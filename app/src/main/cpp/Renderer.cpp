#include "Renderer.h"

#include <game-activity/native_app_glue/android_native_app_glue.h>
#include <GLES3/gl3.h>
#include <memory>
#include <vector>
#include <android/imagedecoder.h>

#include "AndroidOut.h"
#include "Shader.h"
#include "Utility.h"
#include "TextureAsset.h"

// --- 1. BIBLIOTECAS DE REDE E VARIÁVEIS DO UDP ---
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <cstdio>

static float joyX = 0.0f;
static float joyY = 0.0f;
static int udp_sock = -1;
static struct sockaddr_in esp32_addr;
#define ESP_IDF_WIFI_PORT 1234
// -------------------------------------------------
//! executes glGetString and outputs the result to logcat

//! executes glGetString and outputs the result to logcat
#define PRINT_GL_STRING(s) {aout << #s": "<< glGetString(s) << std::endl;}

/*!
 * @brief if glGetString returns a space separated list of elements, prints each one on a new line
 *
 * This works by creating an istringstream of the input c-style string. Then that is used to create
 * a vector -- each element of the vector is a new element in the input string. Finally a foreach
 * loop consumes this and outputs it to logcat using @a aout
 */
#define PRINT_GL_STRING_AS_LIST(s) { \
std::istringstream extensionStream((const char *) glGetString(s));\
std::vector<std::string> extensionList(\
        std::istream_iterator<std::string>{extensionStream},\
        std::istream_iterator<std::string>());\
aout << #s":\n";\
for (auto& extension: extensionList) {\
    aout << extension << "\n";\
}\
aout << std::endl;\
}

//! Color for cornflower blue. Can be sent directly to glClearColor
#define CORNFLOWER_BLUE 100 / 255.f, 149 / 255.f, 237 / 255.f, 1

// Vertex shader, you'd typically load this from assets
static const char *vertex = R"vertex(#version 300 es
in vec3 inPosition;
in vec2 inUV;

out vec2 fragUV;

uniform mat4 uProjection;

void main() {
    fragUV = inUV;
    gl_Position = uProjection * vec4(inPosition, 1.0);
}
)vertex";

// Fragment shader, you'd typically load this from assets
static const char *fragment = R"fragment(#version 300 es
precision mediump float;

in vec2 fragUV;

uniform sampler2D uTexture;

out vec4 outColor;

void main() {
    outColor = texture(uTexture, fragUV);
}
)fragment";

/*!
 * Half the height of the projection matrix. This gives you a renderable area of height 4 ranging
 * from -2 to 2
 */
static constexpr float kProjectionHalfHeight = 2.f;

/*!
 * The near plane distance for the projection matrix. Since this is an orthographic projection
 * matrix, it's convenient to have negative values for sorting (and avoiding z-fighting at 0).
 */
static constexpr float kProjectionNearPlane = -1.f;

/*!
 * The far plane distance for the projection matrix. Since this is an orthographic porjection
 * matrix, it's convenient to have the far plane equidistant from 0 as the near plane.
 */
static constexpr float kProjectionFarPlane = 1.f;

Renderer::~Renderer() {
    // --- 4. FECHAR CONEXÃO AO SAIR ---
    if (udp_sock != -1) {
        close(udp_sock);
        udp_sock = -1;
    }

    if (display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context_ != EGL_NO_CONTEXT) {
            eglDestroyContext(display_, context_);
            context_ = EGL_NO_CONTEXT;
        }
        if (surface_ != EGL_NO_SURFACE) {
            eglDestroySurface(display_, surface_);
            surface_ = EGL_NO_SURFACE;
        }
        eglTerminate(display_);
        display_ = EGL_NO_DISPLAY;
    }
}

void Renderer::render() {
    updateRenderArea();

    if (shaderNeedsNewProjectionMatrix_) {
        float projectionMatrix[16] = {0};
        Utility::buildOrthographicMatrix(projectionMatrix, kProjectionHalfHeight, float(width_) / height_, kProjectionNearPlane, kProjectionFarPlane);
        shader_->setProjectionMatrix(projectionMatrix);
        shaderNeedsNewProjectionMatrix_ = false;
    }

    // --- A MÁGICA VISUAL DO GAMESIR ---
    // Transforma o eixo (que vai de -1.0 a 1.0) em cor válida do OpenGL (0.0 a 1.0)
    float corVermelha = (joyX + 1.0f) / 2.0f;
    float corVerde    = (joyY + 1.0f) / 2.0f;

    // A tela muda de cor em tempo real. O azul deixamos fixo em 0.2f
    glClearColor(corVermelha, corVerde, 0.2f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // O bonequinho do Android foi comentado/apagado daqui para não aparecer mais!
    /*
    if (!models_.empty()) {
        for (const auto &model: models_) {
            shader_->drawModel(model);
        }
    }
    */

    auto swapResult = eglSwapBuffers(display_, surface_);
    assert(swapResult == EGL_TRUE);
}

void Renderer::initRenderer() {
    // Choose your render attributes
    constexpr EGLint attribs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_BLUE_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_RED_SIZE, 8,
            EGL_DEPTH_SIZE, 24,
            EGL_NONE
    };

    // The default display is probably what you want on Android
    auto display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(display, nullptr, nullptr);

    // figure out how many configs there are
    EGLint numConfigs;
    eglChooseConfig(display, attribs, nullptr, 0, &numConfigs);

    // get the list of configurations
    std::unique_ptr<EGLConfig[]> supportedConfigs(new EGLConfig[numConfigs]);
    eglChooseConfig(display, attribs, supportedConfigs.get(), numConfigs, &numConfigs);

    // Find a config we like.
    // Could likely just grab the first if we don't care about anything else in the config.
    // Otherwise hook in your own heuristic
    auto config = *std::find_if(
            supportedConfigs.get(),
            supportedConfigs.get() + numConfigs,
            [&display](const EGLConfig &config) {
                EGLint red, green, blue, depth;
                if (eglGetConfigAttrib(display, config, EGL_RED_SIZE, &red)
                    && eglGetConfigAttrib(display, config, EGL_GREEN_SIZE, &green)
                    && eglGetConfigAttrib(display, config, EGL_BLUE_SIZE, &blue)
                    && eglGetConfigAttrib(display, config, EGL_DEPTH_SIZE, &depth)) {

                    aout << "Found config with " << red << ", " << green << ", " << blue << ", "
                         << depth << std::endl;
                    return red == 8 && green == 8 && blue == 8 && depth == 24;
                }
                return false;
            });

    aout << "Found " << numConfigs << " configs" << std::endl;
    aout << "Chose " << config << std::endl;

    // create the proper window surface
    EGLint format;
    eglGetConfigAttrib(display, config, EGL_NATIVE_VISUAL_ID, &format);
    EGLSurface surface = eglCreateWindowSurface(display, config, app_->window, nullptr);

    // Create a GLES 3 context
    EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, nullptr, contextAttribs);

    // get some window metrics
    auto madeCurrent = eglMakeCurrent(display, surface, surface, context);
    assert(madeCurrent);

    display_ = display;
    surface_ = surface;
    context_ = context;

    // make width and height invalid so it gets updated the first frame in @a updateRenderArea()
    width_ = -1;
    height_ = -1;

    PRINT_GL_STRING(GL_VENDOR);
    PRINT_GL_STRING(GL_RENDERER);
    PRINT_GL_STRING(GL_VERSION);
    PRINT_GL_STRING_AS_LIST(GL_EXTENSIONS);

    shader_ = std::unique_ptr<Shader>(
            Shader::loadShader(vertex, fragment, "inPosition", "inUV", "uProjection"));
    assert(shader_);

    // Note: there's only one shader in this demo, so I'll activate it here. For a more complex game
    // you'll want to track the active shader and activate/deactivate it as necessary
    shader_->activate();

    // setup any other gl related global states
    glClearColor(CORNFLOWER_BLUE);

    // enable alpha globally for now, you probably don't want to do this in a game
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // get some demo models into memory
    createModels();

    // --- 2. INICIALIZAR O SOCKET UDP ---
    udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&esp32_addr, 0, sizeof(esp32_addr));
    esp32_addr.sin_family = AF_INET;
    esp32_addr.sin_port = htons(ESP_IDF_WIFI_PORT);
    inet_pton(AF_INET, "192.168.4.1", &esp32_addr.sin_addr);

    aout << "Socket UDP criado apontando para 192.168.4.1" << std::endl;
    // -----------------------------------
}

void Renderer::updateRenderArea() {
    EGLint width;
    eglQuerySurface(display_, surface_, EGL_WIDTH, &width);

    EGLint height;
    eglQuerySurface(display_, surface_, EGL_HEIGHT, &height);

    if (width != width_ || height != height_) {
        width_ = width;
        height_ = height;
        glViewport(0, 0, width, height);

        // make sure that we lazily recreate the projection matrix before we render
        shaderNeedsNewProjectionMatrix_ = true;
    }
}

/**
 * @brief Create any demo models we want for this demo.
 */
void Renderer::createModels() {
    /*
     * This is a square:
     * 0 --- 1
     * | \   |
     * |  \  |
     * |   \ |
     * 3 --- 2
     */
    std::vector<Vertex> vertices = {
            Vertex(Vector3{1, 1, 0}, Vector2{0, 0}), // 0
            Vertex(Vector3{-1, 1, 0}, Vector2{1, 0}), // 1
            Vertex(Vector3{-1, -1, 0}, Vector2{1, 1}), // 2
            Vertex(Vector3{1, -1, 0}, Vector2{0, 1}) // 3
    };
    std::vector<Index> indices = {
            0, 1, 2, 0, 2, 3
    };

    // loads an image and assigns it to the square.
    //
    // Note: there is no texture management in this sample, so if you reuse an image be careful not
    // to load it repeatedly. Since you get a shared_ptr you can safely reuse it in many models.
    auto assetManager = app_->activity->assetManager;
    auto spAndroidRobotTexture = TextureAsset::loadAsset(assetManager, "android_robot.png");

    // Create a model and put it in the back of the render list.
    models_.emplace_back(vertices, indices, spAndroidRobotTexture);
}

void Renderer::handleInput() {
    auto *inputBuffer = android_app_swap_input_buffers(app_);

    // ====================================================================
    // 1. ATUALIZA OS VALORES APENAS QUANDO O CONTROLE SE MEXER
    // ====================================================================
    if (inputBuffer) {
        // Lê os eixos analógicos
        for (auto i = 0; i < inputBuffer->motionEventsCount; i++) {
            auto &motionEvent = inputBuffer->motionEvents[i];

            if ((motionEvent.source & AINPUT_SOURCE_CLASS_JOYSTICK) == AINPUT_SOURCE_CLASS_JOYSTICK) {
                // Direção no Analógico Esquerdo
                joyX = GameActivityPointerAxes_getAxisValue(&motionEvent.pointers[0], AMOTION_EVENT_AXIS_X);

                // Gatilhos
                float rt = GameActivityPointerAxes_getAxisValue(&motionEvent.pointers[0], AMOTION_EVENT_AXIS_RTRIGGER);
                float lt = GameActivityPointerAxes_getAxisValue(&motionEvent.pointers[0], AMOTION_EVENT_AXIS_LTRIGGER);

                if (rt == 0.0f && lt == 0.0f) {
                    rt = GameActivityPointerAxes_getAxisValue(&motionEvent.pointers[0], AMOTION_EVENT_AXIS_GAS);
                    lt = GameActivityPointerAxes_getAxisValue(&motionEvent.pointers[0], AMOTION_EVENT_AXIS_BRAKE);
                }

                // Atualiza o eixo Y (mesmo que seja zero ao soltar)
                joyY = rt - lt;
            }
        }

        // Lê os gatilhos caso estejam em modo botão digital (clique)
        for (auto i = 0; i < inputBuffer->keyEventsCount; i++) {
            auto &keyEvent = inputBuffer->keyEvents[i];

            if (keyEvent.keyCode == AKEYCODE_BUTTON_R2) {
                joyY = (keyEvent.action == AKEY_EVENT_ACTION_DOWN) ? 1.0f : 0.0f;
            } else if (keyEvent.keyCode == AKEYCODE_BUTTON_L2) {
                joyY = (keyEvent.action == AKEY_EVENT_ACTION_DOWN) ? -1.0f : 0.0f;
            }
        }

        // Limpa os buffers
        android_app_clear_motion_events(inputBuffer);
        android_app_clear_key_events(inputBuffer);
    }

    // ====================================================================
    // 2. ENVIO CONTÍNUO DO PACOTE UDP (RODA EM TODOS OS FRAMES DA TELA)
    // ====================================================================
    // Aplica as zonas mortas
    if (std::abs(joyX) < 0.1f) joyX = 0.0f;
    if (std::abs(joyY) < 0.05f) joyY = 0.0f;

    if (udp_sock != -1) {
        char payload[64];
        snprintf(payload, sizeof(payload), "{\"eixoX\":%.2f,\"eixoY\":%.2f}", joyX, joyY);

        // Dispara o UDP repetidamente usando os últimos valores gravados
        sendto(udp_sock, payload, strlen(payload), 0, (struct sockaddr*)&esp32_addr, sizeof(esp32_addr));

        // Opcional: Descomente a linha abaixo para ver o "metralhador" de pacotes no Logcat
        // aout << "ENVIANDO CONTINUAMENTE -> " << payload << std::endl;
    }
}