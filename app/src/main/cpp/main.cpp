#include <jni.h>

#include <game-activity/native_app_glue/android_native_app_glue.h>
#include <game-activity/GameActivity.h>

#include "AndroidOut.h"
#include "Renderer.h"

extern "C" {

void handle_cmd(android_app *pApp, int32_t cmd) {
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            pApp->userData = new Renderer(pApp);
            break;
        case APP_CMD_TERM_WINDOW:
            if (pApp->userData) {
                auto *pRenderer = reinterpret_cast<Renderer *>(pApp->userData);
                pApp->userData = nullptr;
                delete pRenderer;
            }
            break;
        default:
            break;
    }
}

bool motion_event_filter_func(const GameActivityMotionEvent *motionEvent) {
    auto sourceClass = motionEvent->source & AINPUT_SOURCE_CLASS_MASK;
    return (sourceClass == AINPUT_SOURCE_CLASS_POINTER ||
            sourceClass == AINPUT_SOURCE_CLASS_JOYSTICK);
}

void android_main(struct android_app *pApp) {
    aout << "Welcome to android_main" << std::endl;

    pApp->onAppCmd = handle_cmd;
    android_app_set_motion_event_filter(pApp, motion_event_filter_func);

    // ====================================================================
    // --- ADICIONE ESTAS 4 LINHAS AQUI PARA LIBERAR OS GATILHOS ---
    // ====================================================================
    GameActivityPointerAxes_enableAxis(AMOTION_EVENT_AXIS_RTRIGGER); // Eixo 18
    GameActivityPointerAxes_enableAxis(AMOTION_EVENT_AXIS_LTRIGGER); // Eixo 17
    GameActivityPointerAxes_enableAxis(AMOTION_EVENT_AXIS_GAS);      // Pedal Acelerador (Garantia)
    GameActivityPointerAxes_enableAxis(AMOTION_EVENT_AXIS_BRAKE);    // Pedal Freio (Garantia)
    // ====================================================================

    do {
        bool done = false;
        while (!done) {
            int timeout = 0;
            int events;
            android_poll_source *pSource;
            int result = ALooper_pollOnce(timeout, nullptr, &events,
                                          reinterpret_cast<void**>(&pSource));
            switch (result) {
                case ALOOPER_POLL_TIMEOUT:
                    [[clang::fallthrough]];
                case ALOOPER_POLL_WAKE:
                    done = true;
                    break;
                case ALOOPER_EVENT_ERROR:
                    aout << "ALooper_pollOnce returned an error" << std::endl;
                    break;
                case ALOOPER_POLL_CALLBACK:
                    break;
                default:
                    if (pSource) {
                        pSource->process(pApp, pSource);
                    }
            }
        }

        if (pApp->userData) {
            auto *pRenderer = reinterpret_cast<Renderer *>(pApp->userData);

            // AGORA SIM! Repassa os eventos para o Renderer.cpp fazer a mágica das cores e do UDP
            pRenderer->handleInput();

            // Desenha a tela
            pRenderer->render();
        }

    } while (!pApp->destroyRequested);
}
}