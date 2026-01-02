#include <jni.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "font_data.h" // Include our bitmap font

#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, "SMC_GLES", __VA_ARGS__))

// --- SMC SECURITY TEST ---
__attribute__((noinline)) int target_function() {
    return 0;
}

int run_security_test() {
    void *func_ptr = (void *)target_function;
    size_t page_size = sysconf(_SC_PAGESIZE);
    void *page_start = (void *)((uintptr_t)func_ptr & ~(page_size - 1));

    if (mprotect(page_start, page_size, PROT_READ | PROT_WRITE | PROT_EXEC) == -1) return 0;

    uint32_t *code_ptr = (uint32_t *)func_ptr;
    int patched = 0;
    for (int i = 0; i < 64; i++) {
        if (code_ptr[i] == 0x2a1f03e0) { // mov w0, wzr
            code_ptr[i] = 0x52800020;    // mov w0, #1
            patched = 1;
            break;
        }
    }
    if (!patched) return 0;

    __builtin___clear_cache((char *)func_ptr, (char *)func_ptr + 64);
    mprotect(page_start, page_size, PROT_READ | PROT_EXEC);

    return (target_function() == 1);
}

// --- OPENGL RENDERER ---
struct engine {
    struct android_app* app;
    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;
    int width, height;
    int test_success;

    // GL State
    GLuint program;
    GLuint texture;
    GLint pos_loc, uv_loc, color_loc, sampler_loc;
};

// Simple Shaders
const char* vShaderStr =
    "attribute vec4 vPosition;\n"
    "attribute vec2 vTexCoord;\n"
    "varying vec2 fTexCoord;\n"
    "void main() {\n"
    "  gl_Position = vPosition;\n"
    "  fTexCoord = vTexCoord;\n"
    "}\n";

const char* fShaderStr =
    "precision mediump float;\n"
    "varying vec2 fTexCoord;\n"
    "uniform sampler2D sTexture;\n"
    "uniform vec4 vColor;\n"
    "void main() {\n"
    "  float alpha = texture2D(sTexture, fTexCoord).a;\n"
    "  if (alpha < 0.5) discard;\n" // Simple alpha testing
    "  gl_FragColor = vColor;\n"
    "}\n";

GLuint loadShader(GLenum type, const char *shaderSrc) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &shaderSrc, NULL);
    glCompileShader(shader);
    return shader;
}

void init_gl_resources(struct engine* engine) {
    // 1. Load Shaders
    GLuint vertexShader = loadShader(GL_VERTEX_SHADER, vShaderStr);
    GLuint fragmentShader = loadShader(GL_FRAGMENT_SHADER, fShaderStr);
    engine->program = glCreateProgram();
    glAttachShader(engine->program, vertexShader);
    glAttachShader(engine->program, fragmentShader);
    glLinkProgram(engine->program);
    glUseProgram(engine->program);

    engine->pos_loc = glGetAttribLocation(engine->program, "vPosition");
    engine->uv_loc = glGetAttribLocation(engine->program, "vTexCoord");
    engine->color_loc = glGetUniformLocation(engine->program, "vColor");
    engine->sampler_loc = glGetUniformLocation(engine->program, "sTexture");

    // 2. Create Font Texture
    // We expand 1-bit font data to 8-bit alpha texture
    // 128 chars, 8x8 pixels each. Let's arrange them in a long strip or just logical processing.
    // For simplicity, we make a texture atlas: 128x8 pixels (1 char tall, 128 wide? No, GL restrictions).
    // Safer: 8x1024 (1 char wide, 128 tall).

    unsigned char *texBuffer = (unsigned char*)malloc(8 * 1024);
    memset(texBuffer, 0, 8 * 1024);

    // Convert 1-bit font_data to 8-bit alpha
    // font_data is just raw bytes.
    // Row 0 of char 0 is byte 0.
    for (int i = 0; i < 1024; i++) { // For every byte (row of a char)
        unsigned char byte = font_8x8[i];
        for (int bit = 0; bit < 8; bit++) {
            // Expand bits to pixels
            if (byte & (1 << (7-bit))) {
                texBuffer[i * 8 + bit] = 255; 
            } else {
                texBuffer[i * 8 + bit] = 0;
            }
        }
    }

    glGenTextures(1, &engine->texture);
    glBindTexture(GL_TEXTURE_2D, engine->texture);
    // 8 pixels wide, 1024 pixels tall (all chars stacked vertically)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, 8, 1024, 0, GL_ALPHA, GL_UNSIGNED_BYTE, texBuffer);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    free(texBuffer);
}

void draw_text(struct engine* engine, const char* text, float x, float y, float scale) {
    // Map screen coordinates (-1 to 1)
    // Scale is roughly char width
    float charW = 0.1f * scale; // Width in NDC
    float charH = 0.1f * scale * (engine->width / (float)engine->height); // Adjust aspect

    float cursorX = x;
    float cursorY = y;

    glUseProgram(engine->program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, engine->texture);
    glUniform1i(engine->sampler_loc, 0);

    // White text
    glUniform4f(engine->color_loc, 1.0f, 1.0f, 1.0f, 1.0f);

    glEnableVertexAttribArray(engine->pos_loc);
    glEnableVertexAttribArray(engine->uv_loc);

    while (*text) {
        unsigned char c = (unsigned char)*text;
        if (c < 32 || c > 127) c = 32; // basic mapping
        int index = c - 32; // Offset in our font array (space is first)

        // UV coordinates for the char in our 8x1024 strip
        // U is always 0.0 to 1.0
        // V ranges based on index. Each char is 8 pixels tall in a 1024 texture.
        // 8/1024 = 1/128 step.
        float vStep = 1.0f / 128.0f;
        float vTop = index * vStep;
        float vBot = vTop + vStep;

        GLfloat verts[] = {
            cursorX, cursorY, 
            cursorX + charW, cursorY,
            cursorX, cursorY - charH,
            cursorX + charW, cursorY - charH
        };

        GLfloat uvs[] = {
            0.0f, vTop,
            1.0f, vTop,
            0.0f, vBot,
            1.0f, vBot
        };

        glVertexAttribPointer(engine->pos_loc, 2, GL_FLOAT, GL_FALSE, 0, verts);
        glVertexAttribPointer(engine->uv_loc, 2, GL_FLOAT, GL_FALSE, 0, uvs);

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        cursorX += charW;
        text++;
    }

    glDisableVertexAttribArray(engine->pos_loc);
    glDisableVertexAttribArray(engine->uv_loc);
}

static int engine_init_display(struct engine* engine) {
    const EGLint attribs[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_BLUE_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_RED_SIZE, 8, EGL_NONE };
    EGLint format, numConfigs;
    EGLConfig config;

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(display, 0, 0);
    eglChooseConfig(display, attribs, &config, 1, &numConfigs);
    eglGetConfigAttrib(display, config, EGL_NATIVE_VISUAL_ID, &format);
    ANativeWindow_setBuffersGeometry(engine->app->window, 0, 0, format);

    EGLSurface surface = eglCreateWindowSurface(display, config, engine->app->window, NULL);
    const EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext context = eglCreateContext(display, config, NULL, context_attribs);

    if (eglMakeCurrent(display, surface, surface, context) == EGL_FALSE) return -1;

    engine->display = display;
    engine->context = context;
    engine->surface = surface;
    eglQuerySurface(display, surface, EGL_WIDTH, &engine->width);
    eglQuerySurface(display, surface, EGL_HEIGHT, &engine->height);

    // Load Shaders & Texture
    init_gl_resources(engine);

    return 0;
}

static void engine_draw_frame(struct engine* engine) {
    if (engine->display == NULL) return;

    // Background Color
    if (engine->test_success) glClearColor(0.0f, 0.5f, 0.0f, 1.0f); // Dark Green
    else glClearColor(0.5f, 0.0f, 0.0f, 1.0f); // Dark Red

    glClear(GL_COLOR_BUFFER_BIT);

    // Draw Status Text
    if (engine->test_success) {
        draw_text(engine, "SMC SUCCESS!", -0.8f, 0.2f, 1.0f);
        draw_text(engine, "MEMORY PATCHED", -0.8f, 0.0f, 0.7f);
    } else {
        draw_text(engine, "SMC FAILED", -0.8f, 0.0f, 1.0f);
    }

    eglSwapBuffers(engine->display, engine->surface);
}

static void engine_term_display(struct engine* engine) {
    if (engine->display != EGL_NO_DISPLAY) {
        eglMakeCurrent(engine->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (engine->context != EGL_NO_CONTEXT) eglDestroyContext(engine->display, engine->context);
        if (engine->surface != EGL_NO_SURFACE) eglDestroySurface(engine->display, engine->surface);
        eglTerminate(engine->display);
    }
    engine->display = EGL_NO_DISPLAY;
    engine->context = EGL_NO_CONTEXT;
    engine->surface = EGL_NO_SURFACE;
}

static void engine_handle_cmd(struct android_app* app, int32_t cmd) {
    struct engine* engine = (struct engine*)app->userData;
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            if (engine->app->window != NULL) {
                engine_init_display(engine);
                engine_draw_frame(engine);
            }
            break;
        case APP_CMD_TERM_WINDOW:
            engine_term_display(engine);
            break;
    }
}

void android_main(struct android_app* state) {
    struct engine engine;
    memset(&engine, 0, sizeof(engine));
    state->userData = &engine;
    state->onAppCmd = engine_handle_cmd;
    engine.app = state;

    engine.test_success = run_security_test();

    while (1) {
        int ident, events;
        struct android_poll_source* source;
        while ((ident = ALooper_pollOnce(0, NULL, &events, (void**)&source)) >= 0) {
            if (source != NULL) source->process(state, source);
            if (state->destroyRequested != 0) {
                engine_term_display(&engine);
                return;
            }
        }
        engine_draw_frame(&engine);
    }
}
