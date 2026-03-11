#include <chrono>
#include <atomic>

static std::chrono::steady_clock::time_point lastSaveTime = std::chrono::steady_clock::now();
static const std::chrono::milliseconds SAVE_DEBOUNCE_MS(1000); // 1s debounce


#include <iostream>
#include <string>
#include <algorithm> // Для std::min, std::max
#include <cmath>     // For sqrtf in joystick logic
#include <vector>
#include <fstream>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#ifdef __ANDROID__
#include "SDL.h"
#include "SDL_mixer.h"
#else
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#endif

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

static bool introPlayedOnce = false;

//Global 
    const int SCALE = 2;
    const int spriteSize = 32;
    const int drawnSpriteSize = spriteSize * SCALE;
    const int screenWidth = 1280; // Width 1280 (HD standard)
    const int FIELD_HEIGHT = 690; // Height of the game field (720 - 30 for the interface)
    const int HUD_HEIGHT = 30;    // Height of the status bar
    const int WINDOW_HEIGHT = FIELD_HEIGHT + HUD_HEIGHT; // Total window height

    const Uint32 FRAME_TIME = 16; // 16 ms = 60 FPS
    const Uint32 FRAME_DURATION = 180; // 120 ms /animation
    const int MAXANIMALS=20;//maximum count of animals on field

    const int creatureJanitor=0;
    const int creatureCat=1;
    const int creatureDog=2;
    const int objectPoop=3; // Assuming the poop sprite follows
    const int MAX_POOPS=100;
    const int CLEAN_TIME = 60; // Cleaning time in frames (1 second at 60 FPS)

    // New, more balanced system for calculating poop lifetime
    const int POOP_BASE_LIFETIME = 360; // ~6 sec: time to cross screen (256) + 1 dog panic (40) + buffer (64)
    const int POOP_TIME_PER_ANIMAL = 90; // ~1.5 sec: average time to travel to the next poop

    const int FEAR_TIME = 40; // Panic (fleeing) time in frames
    const int JANITOR_SPEED = 3; // Janitor speed (pixels per frame)
    const int JANITOR_BRAVE_RADIUS = 64; // Janitor's courage radius (2 tiles)

    //directions for characters
    const int dDown=1;
    const int dUp=0;
    const int dLeft=2;
    const int dRight=3; 
    

    SDL_Renderer* renderer = nullptr;
    SDL_Window* window = nullptr; // Made global for access in updateInput
    SDL_Texture* texture = nullptr;
    SDL_Texture* backgroundTexture = nullptr; // Texture for the background
    SDL_Texture* numbersTexture = nullptr;
    SDL_Texture* beginTexture = nullptr;
    SDL_Texture* diffTexture = nullptr;
    SDL_Texture* gameOverTexture = nullptr;
    SDL_Texture* scoreTableTexture = nullptr;

    Mix_Chunk* pooSound = nullptr;
    Mix_Chunk* sweepSound = nullptr;
    Mix_Chunk* afraidSound = nullptr;
    Mix_Chunk* catSound = nullptr;
    Mix_Chunk* walkingSound = nullptr;
    Mix_Chunk* lostLiveSound = nullptr;
    Mix_Chunk* levelUpSound = nullptr;

    // For virtual joystick on touch devices
    bool joystickActive = false;
    float joystickBaseX = 0.0f, joystickBaseY = 0.0f;
    float joystickKnobX = 0.0f, joystickKnobY = 0.0f;
    const int JOYSTICK_BASE_RADIUS = 60;
    const int JOYSTICK_KNOB_RADIUS = 30;
    const int JOYSTICK_MAX_OFFSET = 40; // Max distance knob can move from base
    
    std::vector<int> highScores;

// Video globals
plm_t* plm = nullptr;
SDL_Texture* videoTexture = nullptr;
Uint8* videoPixelBuffer = nullptr;
SDL_AudioStream* audioStream = nullptr;

enum GameStateEnum {
    STATE_INIT,
    STATE_INTRO,
    STATE_BEGIN_SCREEN,
    STATE_DIFFICULTY_SELECT,
    STATE_GAMEPLAY,
    STATE_GAME_OVER,
    STATE_HIGH_SCORES,
    STATE_EXIT
};

// Forward declaration
void reset_game_state();

//functions

std::string getHighScorePath() {
#ifdef __EMSCRIPTEN__
    // В веб-версии используем путь в виртуальной файловой системе, которая будет сохраняться.
    return "/gamedata/highscores.txt";
#else
    char* basePath = SDL_GetPrefPath("DvornikGame", "HighScores");
    if (basePath) {
        std::string path = std::string(basePath) + "highscores.txt";
        SDL_free(basePath);
        return path;
    }
    return "highscores.txt";
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE // Позволяет вызывать эту функцию из JavaScript
#endif
void loadHighScores() {
    highScores.clear();
    std::string path = getHighScorePath();
    std::ifstream inFile(path);
    int score;
    if (inFile.is_open()) {
        while (inFile >> score) {
            highScores.push_back(score);
        }
        inFile.close();
    }
    std::sort(highScores.rbegin(), highScores.rend());
    if (highScores.size() > 5) highScores.resize(5);
}

void saveHighScores() {
    std::string path = getHighScorePath();
    std::ofstream outFile(path);
    if (outFile.is_open()) {
        for (int score : highScores) {
            outFile << score << std::endl;
        }
        outFile.close();
#ifdef __EMSCRIPTEN__
        // Запускаем асинхронную синхронизацию немедленно. Это наш лучший шанс сохранить данные,
        // так как обработчик 'beforeunload' может быть ненадежным в iframe на itch.io.
        EM_ASM(FS.syncfs(false, function(err) { if(err) { console.error('Failed to save high scores:', err); } else { console.log('High scores saved to persistent storage.'); } }));
#endif
    }
}

void updateHighScores(int newScore) {
    if (newScore > 0) {
        highScores.push_back(newScore);
        std::sort(highScores.rbegin(), highScores.rend());
        if (highScores.size() > 5) highScores.resize(5);
        saveHighScores();
    }
}

struct GameState {
    GameStateEnum currentState = STATE_INIT;
    int POOP_LIFETIME = 0;
    int animTick = 0;
    int direction = 0;
    Uint32 lastAnimTime;
    int x = 100;
    int y = 100;
    int moveX = 0;
    int moveY = 0;
    int walkingChannel = -1;
    int animalCount = 4;
    const int diffHard = 0;
    const int diffNormal = 1;
    const int diffEasy = 2;
    int cleanTimer = 0;
    int cleanStartX = 0;
    int cleanStartY = 0;
    int targetPoopIndex = -1;
    int fearTimer = 0;
    int forcedMoveX = 0;
    int forcedMoveY = 0;
    int penaltyCount = 0;
    int levelUpTimer = 0;
    int lives = 3;
    int coins = 0;
    int cleanPoops = 0;
    int level = 1;
    int poopsToNextLevel = 5;
    const int MAX_LEVEL = 20;
    bool gameWon = false;
    bool isPaused = false;
    bool isRunning = true;
    SDL_GameController* gamepad = nullptr;
    int lastLives = -1, lastScore = -1, lastLevel = -1, lastPoopsToNextLevel = -1;
    int currentDifficulty = diffNormal;
    int scoreMultiplier = 1;
    bool scoresUpdated = false;
    bool audioContextResumed = false;

    struct animal {
        int x = 0;
        int y = 0;
        int dx = 0;
        int dy = 0;
        int type = 0;
        int direction = 0;
        int moveTimer = 0;
    } animals[MAXANIMALS];

    struct Poop {
        int x;
        int y;
        bool active;
        int lifeTimer;
        bool hasFlies;
        bool penaltyApplied;
    } poops[MAX_POOPS];

    struct TitleAnimal { int x, y, dx, dy, type, direction, moveTimer; } titleAnimals[4];
    int titleAnimTick = 0;
    Uint32 titleLastAnimTime = 0;
};

GameState* gameState = nullptr;

void updateInput(SDL_GameController* gamepad, int& moveX, int& moveY)
{
    moveX = 0;
    moveY = 0;
    const Uint8* keys = SDL_GetKeyboardState(NULL);
    if (keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_W]) moveY -= 1;
    if (keys[SDL_SCANCODE_DOWN] || keys[SDL_SCANCODE_S]) moveY += 1;
    if (keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_A]) moveX -= 1;
    if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D]) moveX += 1;
    if (gamepad) {
        if (SDL_GameControllerGetButton(gamepad, SDL_CONTROLLER_BUTTON_DPAD_UP)) moveY -= 1;
        if (SDL_GameControllerGetButton(gamepad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) moveY += 1;
        if (SDL_GameControllerGetButton(gamepad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) moveX -= 1;
        if (SDL_GameControllerGetButton(gamepad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) moveX += 1;
        int deadZone = 10000;
        Sint16 axisX = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_LEFTX);
        Sint16 axisY = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_LEFTY);
        if (axisX < -deadZone) moveX = -1; else if (axisX > deadZone) moveX = 1;
        if (axisY < -deadZone) moveY = -1; else if (axisY > deadZone) moveY = 1;
    }
    if (joystickActive) {
        float deltaX = joystickKnobX - joystickBaseX;
        float deltaY = joystickKnobY - joystickBaseY;
        const float deadZone = JOYSTICK_MAX_OFFSET * 0.2f;
        if (SDL_fabsf(deltaX) > deadZone) moveX = (deltaX > 0) ? 1 : -1;
        if (SDL_fabsf(deltaY) > deadZone) moveY = (deltaY > 0) ? 1 : -1;
    }
    if (moveX > 1) moveX = 1; if (moveX < -1) moveX = -1;
    if (moveY > 1) moveY = 1; if (moveY < -1) moveY = -1;
}

void drawCreature (int creatureType,int animTick,int direction, int x, int y) {
    SDL_Rect sourceRect = {(creatureType*4+animTick) * spriteSize, direction * spriteSize, spriteSize, spriteSize};
    SDL_Rect destRect = {x, y + HUD_HEIGHT, drawnSpriteSize, drawnSpriteSize};
    SDL_RenderCopy(renderer, texture, &sourceRect, &destRect);
}

void drawCircle(SDL_Renderer* renderer, int centerX, int centerY, int radius) {
    for (int w = 0; w < radius * 2; w++) for (int h = 0; h < radius * 2; h++) {
        int dx = radius - w; int dy = radius - h;
        if ((dx * dx + dy * dy) <= (radius * radius)) SDL_RenderDrawPoint(renderer, centerX + dx, centerY + dy);
    }
}

void drawJoystick() {
    if (!joystickActive) return;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 128, 128, 128, 100);
    drawCircle(renderer, (int)joystickBaseX, (int)joystickBaseY, JOYSTICK_BASE_RADIUS);
    SDL_SetRenderDrawColor(renderer, 80, 80, 80, 150);
    drawCircle(renderer, (int)joystickKnobX, (int)joystickKnobY, JOYSTICK_KNOB_RADIUS);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

void drawHeart(SDL_Renderer* renderer, float x, float y, float size) {
    SDL_SetRenderDrawColor(renderer, 255, 50, 50, 255);
    float u = size / 8.0f;
    SDL_Rect rects[] = {
        {(int)(x + 1*u), (int)(y + 0*u), (int)(2*u), (int)(1*u)}, {(int)(x + 5*u), (int)(y + 0*u), (int)(2*u), (int)(1*u)},
        {(int)(x + 0*u), (int)(y + 1*u), (int)(8*u), (int)(2*u)}, {(int)(x + 1*u), (int)(y + 3*u), (int)(6*u), (int)(1*u)},
        {(int)(x + 2*u), (int)(y + 4*u), (int)(4*u), (int)(1*u)}, {(int)(x + 3*u), (int)(y + 5*u), (int)(2*u), (int)(1*u)}
    };
    SDL_RenderFillRects(renderer, rects, 6);
}

void drawNumber(int number, int x, int y, int h, SDL_Texture* numTexture) {
    if (!numTexture) return;
    const int SRC_DIGIT_WIDTH = 274; const int SRC_DIGIT_HEIGHT = 385;
    float aspect = (float)SRC_DIGIT_WIDTH / (float)SRC_DIGIT_HEIGHT;
    int w = (int)(h * aspect);
    std::string numStr = std::to_string(number);
    int currentX = x;
    for (char c : numStr) {
        int digit = c - '0';
        if (digit < 0 || digit > 9) continue;
        SDL_Rect srcRect = { digit * SRC_DIGIT_WIDTH, 0, SRC_DIGIT_WIDTH, SRC_DIGIT_HEIGHT };
        SDL_Rect destRect = { currentX, y, w, h };
        SDL_RenderCopy(renderer, numTexture, &srcRect, &destRect);
        currentX += w;
    }
}

void my_video_callback(plm_t *plm, plm_frame_t *frame, void *user) {
    if (videoPixelBuffer) {
        plm_frame_to_rgb(frame, videoPixelBuffer, frame->width * 3);
    }
}

void my_audio_callback(plm_t *plm, plm_samples_t *samples, void *user) {
    if (audioStream) {
        SDL_AudioStreamPut(audioStream, samples->interleaved, samples->count * 2 * sizeof(float));
    }
}

void music_player(void *udata, Uint8 *stream_data, int len) {
    if (audioStream) {
        int available = SDL_AudioStreamAvailable(audioStream);
        if (available > 0) {
            int read = SDL_AudioStreamGet(audioStream, stream_data, len);
            if (read < len) {
                memset(stream_data + read, 0, len - read);
            }
        } else {
            memset(stream_data, 0, len);
        }
    } else {
        memset(stream_data, 0, len);
    }
}

plm_t* load_plm_video(const char* filename) {
    plm_t* p = plm_create_with_filename(filename);
    if (!p) return nullptr;
    
    if (!plm_get_num_video_streams(p)) {
        plm_destroy(p);
        return nullptr;
    }

    plm_set_video_decode_callback(p, my_video_callback, nullptr);
    plm_set_audio_decode_callback(p, my_audio_callback, nullptr);
    plm_set_loop(p, 0);
    plm_set_audio_enabled(p, 1);
    plm_set_video_enabled(p, 1);
    plm_set_audio_lead_time(p, 0.2);
    
    return p;
}

void loop_intro() {
    // Эта часть выполняется сразу на десктопе или после первого клика в вебе.
    if (!plm) {
        plm = load_plm_video("start.mpg");
        if (!plm) {
            gameState->currentState = STATE_BEGIN_SCREEN;
            return;
        }
        int w = plm_get_width(plm);
        int h = plm_get_height(plm);
        videoTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, w, h);
        videoPixelBuffer = (Uint8*)malloc(w * h * 3);
        
        int freq = 44100; Uint16 format = AUDIO_S16SYS; int channels = 2;
        Mix_QuerySpec(&freq, &format, &channels);
        audioStream = SDL_NewAudioStream(AUDIO_F32SYS, 2, plm_get_samplerate(plm), format, channels, freq);
        Mix_HookMusic(music_player, nullptr);
    }

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) { gameState->isRunning = false; }
        else if (event.type == SDL_KEYDOWN || event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_FINGERDOWN) {
             gameState->currentState = STATE_DIFFICULTY_SELECT;
        } else if (event.type == SDL_CONTROLLERDEVICEADDED) {
            if (!gameState->gamepad) {
                gameState->gamepad = SDL_GameControllerOpen(event.cdevice.which);
            }
        } else if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
            if (gameState->gamepad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gameState->gamepad)) == event.cdevice.which) {
                SDL_GameControllerClose(gameState->gamepad);
                gameState->gamepad = nullptr;
            }
        }
    }

    plm_decode(plm, FRAME_TIME / 1000.0f);

    if (videoTexture && videoPixelBuffer) {
        SDL_UpdateTexture(videoTexture, NULL, videoPixelBuffer, plm_get_width(plm) * 3);
    }

    SDL_RenderClear(renderer);
    if (videoTexture) SDL_RenderCopy(renderer, videoTexture, NULL, NULL);
    SDL_RenderPresent(renderer);

    if (plm_has_ended(plm) || gameState->currentState != STATE_INTRO) {
        if (plm) { plm_destroy(plm); plm = nullptr; }
        if (videoTexture) { SDL_DestroyTexture(videoTexture); videoTexture = nullptr; }
        if (videoPixelBuffer) { free(videoPixelBuffer); videoPixelBuffer = nullptr; }
        if (audioStream) { SDL_FreeAudioStream(audioStream); audioStream = nullptr; }
        Mix_HookMusic(NULL, NULL);
        introPlayedOnce = true;
        if (gameState->currentState == STATE_INTRO) gameState->currentState = STATE_DIFFICULTY_SELECT;
    }
}

void loop_begin_screen() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) { gameState->isRunning = false; return; }
        if (event.type == SDL_KEYDOWN || event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_FINGERDOWN) {
#ifdef __EMSCRIPTEN__
            if (!gameState->audioContextResumed) {
                EM_ASM({
                    if (typeof(Module.SDL2) !== 'undefined' && Module.SDL2.audioContext && Module.SDL2.audioContext.state === 'suspended') {
                        Module.SDL2.audioContext.resume().catch(function(e) { console.error("Could not resume audio context: " + e); });
                    }
                });
                gameState->audioContextResumed = true;
            }
#endif
            if (!introPlayedOnce) {
                gameState->currentState = STATE_INTRO;
            } else {
                gameState->currentState = STATE_DIFFICULTY_SELECT;
            }
            return;
        } else if (event.type == SDL_CONTROLLERDEVICEADDED) {
            if (!gameState->gamepad) {
                gameState->gamepad = SDL_GameControllerOpen(event.cdevice.which);
            }
        } else if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
            if (gameState->gamepad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gameState->gamepad)) == event.cdevice.which) {
                SDL_GameControllerClose(gameState->gamepad);
                gameState->gamepad = nullptr;
            }
        }
    }
    for (int i = 0; i < 4; i++) {
        if (gameState->titleAnimals[i].moveTimer > 0) { gameState->titleAnimals[i].moveTimer--; } 
        else {
            gameState->titleAnimals[i].moveTimer = rand() % 60 + 30; int action = rand() % 5;
            gameState->titleAnimals[i].dx = 0; gameState->titleAnimals[i].dy = 0;
            if (action == 1) gameState->titleAnimals[i].dy = -1; if (action == 2) gameState->titleAnimals[i].dy = 1;
            if (action == 3) gameState->titleAnimals[i].dx = -1; if (action == 4) gameState->titleAnimals[i].dx = 1;
        }
        gameState->titleAnimals[i].x += gameState->titleAnimals[i].dx; gameState->titleAnimals[i].y += gameState->titleAnimals[i].dy;
        if (gameState->titleAnimals[i].x < 0) { gameState->titleAnimals[i].x = 0; gameState->titleAnimals[i].moveTimer = 0; }
        if (gameState->titleAnimals[i].y < 0) { gameState->titleAnimals[i].y = 0; gameState->titleAnimals[i].moveTimer = 0; }
        if (gameState->titleAnimals[i].x > screenWidth - drawnSpriteSize) { gameState->titleAnimals[i].x = screenWidth - drawnSpriteSize; gameState->titleAnimals[i].moveTimer = 0; }
        if (gameState->titleAnimals[i].y > FIELD_HEIGHT - drawnSpriteSize) { gameState->titleAnimals[i].y = FIELD_HEIGHT - drawnSpriteSize; gameState->titleAnimals[i].moveTimer = 0; }
        if (gameState->titleAnimals[i].dy < 0) gameState->titleAnimals[i].direction = dUp;
        else if (gameState->titleAnimals[i].dy > 0) gameState->titleAnimals[i].direction = dDown;
        else if (gameState->titleAnimals[i].dx < 0) gameState->titleAnimals[i].direction = dLeft;
        else if (gameState->titleAnimals[i].dx > 0) gameState->titleAnimals[i].direction = dRight;
    }
    if (SDL_GetTicks() - gameState->titleLastAnimTime > FRAME_DURATION) {
        gameState->titleAnimTick = (gameState->titleAnimTick + 1) % 4; gameState->titleLastAnimTime = SDL_GetTicks();
    }
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, beginTexture, NULL, NULL);
    for (int i = 0; i < 4; i++) {
        drawCreature(gameState->titleAnimals[i].type, gameState->titleAnimTick, gameState->titleAnimals[i].direction, gameState->titleAnimals[i].x, gameState->titleAnimals[i].y);
    }
    SDL_RenderPresent(renderer);
}

void loop_difficulty_select() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) { gameState->isRunning = false; }
        if (event.type == SDL_KEYDOWN) {
            switch (event.key.keysym.sym) {
                case SDLK_1: gameState->currentDifficulty = gameState->diffEasy; gameState->currentState = STATE_GAMEPLAY; break;
                case SDLK_2: gameState->currentDifficulty = gameState->diffNormal; gameState->currentState = STATE_GAMEPLAY; break;
                case SDLK_3: gameState->currentDifficulty = gameState->diffHard; gameState->currentState = STATE_GAMEPLAY; break;
            }
        }
        if (event.type == SDL_CONTROLLERBUTTONDOWN) {
            if (gameState->gamepad) {
                switch (event.cbutton.button) {
                    case SDL_CONTROLLER_BUTTON_A: gameState->currentDifficulty = gameState->diffEasy; gameState->currentState = STATE_GAMEPLAY; break;
                    case SDL_CONTROLLER_BUTTON_B: gameState->currentDifficulty = gameState->diffNormal; gameState->currentState = STATE_GAMEPLAY; break;
                    case SDL_CONTROLLER_BUTTON_X: gameState->currentDifficulty = gameState->diffHard; gameState->currentState = STATE_GAMEPLAY; break;
                }
            }
        }
        if (event.type == SDL_FINGERDOWN) {
            float touchX = event.tfinger.x;
            if (touchX < 0.33f) { gameState->currentDifficulty = gameState->diffEasy; gameState->currentState = STATE_GAMEPLAY; } 
            else if (touchX < 0.66f) { gameState->currentDifficulty = gameState->diffNormal; gameState->currentState = STATE_GAMEPLAY; } 
            else { gameState->currentDifficulty = gameState->diffHard; gameState->currentState = STATE_GAMEPLAY; }
        }
        else if (event.type == SDL_CONTROLLERDEVICEADDED) {
            if (!gameState->gamepad) {
                gameState->gamepad = SDL_GameControllerOpen(event.cdevice.which);
            }
        } else if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
            if (gameState->gamepad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gameState->gamepad)) == event.cdevice.which) {
                SDL_GameControllerClose(gameState->gamepad);
                gameState->gamepad = nullptr;
            }
        }
    }
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, diffTexture, NULL, NULL);
    SDL_RenderPresent(renderer);
    if (gameState->currentState == STATE_GAMEPLAY) {
        int difficultyBonus = 0;
        if (gameState->currentDifficulty == gameState->diffNormal) { difficultyBonus = 600; gameState->scoreMultiplier = 2; }
        else if (gameState->currentDifficulty == gameState->diffEasy) { difficultyBonus = 1200; }
        else if (gameState->currentDifficulty == gameState->diffHard) { gameState->scoreMultiplier = 3; }
        gameState->POOP_LIFETIME = POOP_BASE_LIFETIME + (gameState->animalCount * POOP_TIME_PER_ANIMAL) + difficultyBonus;
        gameState->lastAnimTime = SDL_GetTicks();
    }
}

void loop_gameplay() {
    SDL_Event event;
    bool isJanitorMoving = false;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) { gameState->isRunning = false; }
        if (event.type == SDL_FINGERDOWN) {
            if (event.tfinger.y * WINDOW_HEIGHT > HUD_HEIGHT) {
                joystickActive = true;
                joystickBaseX = event.tfinger.x * screenWidth; joystickBaseY = event.tfinger.y * WINDOW_HEIGHT;
                joystickKnobX = joystickBaseX; joystickKnobY = joystickBaseY;
            }
        }
        if (event.type == SDL_FINGERMOTION) {
            if (joystickActive) {
                joystickKnobX = event.tfinger.x * screenWidth; joystickKnobY = event.tfinger.y * WINDOW_HEIGHT;
                float deltaX = joystickKnobX - joystickBaseX; float deltaY = joystickKnobY - joystickBaseY;
                float distance = SDL_sqrtf(deltaX * deltaX + deltaY * deltaY);
                if (distance > JOYSTICK_MAX_OFFSET) {
                    float ratio = JOYSTICK_MAX_OFFSET / distance;
                    joystickKnobX = joystickBaseX + deltaX * ratio; joystickKnobY = joystickBaseY + deltaY * ratio;
                }
            }
        }
        if (event.type == SDL_FINGERUP) { joystickActive = false; }
        if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
            if (event.key.keysym.sym == SDLK_SPACE || event.key.keysym.sym == SDLK_RETURN) {
                gameState->isPaused = !gameState->isPaused;
                if(gameState->isPaused && gameState->walkingChannel != -1) { Mix_HaltChannel(gameState->walkingChannel); gameState->walkingChannel = -1; }
            } else if (event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_AC_BACK) {
                gameState->currentState = STATE_GAME_OVER;
            }
        }
        if (event.type == SDL_CONTROLLERBUTTONDOWN) {
            if (event.cbutton.button == SDL_CONTROLLER_BUTTON_START) {
                gameState->isPaused = !gameState->isPaused;
                 if(gameState->isPaused && gameState->walkingChannel != -1) { Mix_HaltChannel(gameState->walkingChannel); gameState->walkingChannel = -1; }
            } else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) {
                gameState->currentState = STATE_GAME_OVER;
            }
        } else if (event.type == SDL_CONTROLLERDEVICEADDED) {
            if (!gameState->gamepad) {
                gameState->gamepad = SDL_GameControllerOpen(event.cdevice.which);
            }
        } else if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
            if (gameState->gamepad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gameState->gamepad)) == event.cdevice.which) {
                SDL_GameControllerClose(gameState->gamepad);
                gameState->gamepad = nullptr;
            }
        }
    }

    if (gameState->lives <= 0) { gameState->currentState = STATE_GAME_OVER; }

    if (gameState->currentState == STATE_GAME_OVER) return;

    if (!gameState->isPaused) {
        Uint32 start = SDL_GetTicks();
        if (gameState->levelUpTimer > 0) { gameState->levelUpTimer--; }
        if (gameState->fearTimer > 0) {
            gameState->fearTimer--; isJanitorMoving = true;
            gameState->moveX = gameState->forcedMoveX * (JANITOR_SPEED * 2); gameState->moveY = gameState->forcedMoveY * (JANITOR_SPEED * 2);
            if (gameState->moveY < 0) gameState->direction = dUp; else if (gameState->moveY > 0) gameState->direction = dDown;
            else if (gameState->moveX < 0) gameState->direction = dLeft; else if (gameState->moveX > 0) gameState->direction = dRight;
            gameState->x += gameState->moveX; gameState->y += gameState->moveY;
        } else if (gameState->cleanTimer > 0) {
            if (gameState->targetPoopIndex != -1) {
                int targetX = gameState->poops[gameState->targetPoopIndex].x; int targetY = gameState->poops[gameState->targetPoopIndex].y;
                int approachSpeed = JANITOR_SPEED; bool moving = false;
                if (gameState->x < targetX) { gameState->x = (gameState->x + approachSpeed > targetX) ? targetX : gameState->x + approachSpeed; moving = true; }
                else if (gameState->x > targetX) { gameState->x = (gameState->x - approachSpeed < targetX) ? targetX : gameState->x - approachSpeed; moving = true; }
                if (gameState->y < targetY) { gameState->y = (gameState->y + approachSpeed > targetY) ? targetY : gameState->y + approachSpeed; moving = true; }
                else if (gameState->y > targetY) { gameState->y = (gameState->y - approachSpeed < targetY) ? targetY : gameState->y - approachSpeed; moving = true; }
                if (moving) {
                    isJanitorMoving = true;
                    if (SDL_abs(targetY - gameState->y) > SDL_abs(targetX - gameState->x)) { gameState->direction = (targetY > gameState->y) ? dDown : dUp; } 
                    else { gameState->direction = (targetX > gameState->x) ? dRight : dLeft; }
                } else {
                    if (gameState->cleanTimer == CLEAN_TIME) Mix_PlayChannel(-1, sweepSound, 0);
                    gameState->cleanTimer--;
                    if ((gameState->cleanTimer / 8) % 2 == 0) gameState->direction = dLeft; else gameState->direction = dRight;
                    if (gameState->cleanTimer == 0) {
                        gameState->poops[gameState->targetPoopIndex].active = false; gameState->coins += gameState->scoreMultiplier;
                        if (!gameState->poops[gameState->targetPoopIndex].hasFlies) gameState->cleanPoops++;
                        gameState->targetPoopIndex = -1; gameState->poopsToNextLevel--;
                        if (gameState->poopsToNextLevel <= 0) {
                            if (gameState->level >= gameState->MAX_LEVEL) { gameState->gameWon = true; gameState->currentState = STATE_GAME_OVER; }
                            else {
                                gameState->level++; gameState->poopsToNextLevel = 5 + (gameState->level * 2);
                                gameState->levelUpTimer = 90; if (gameState->lives < 3) gameState->lives++;
                                gameState->coins += 50 * gameState->scoreMultiplier;
                                if (levelUpSound) Mix_PlayChannel(-1, levelUpSound, 0);
                                if (gameState->animalCount < MAXANIMALS) gameState->animalCount++;
                                gameState->POOP_LIFETIME = POOP_BASE_LIFETIME + (gameState->animalCount * POOP_TIME_PER_ANIMAL);
                            }
                        }
                    }
                }
            } else { gameState->cleanTimer = 0; }
        } else {
            updateInput(gameState->gamepad, gameState->moveX, gameState->moveY);
            if (gameState->moveX != 0 || gameState->moveY != 0) isJanitorMoving = true;
            if (gameState->moveY < 0) gameState->direction = dUp; else if (gameState->moveY > 0) gameState->direction = dDown;
            else if (gameState->moveX < 0) gameState->direction = dLeft; else if (gameState->moveX > 0) gameState->direction = dRight;
            float moveSpeed = static_cast<float>(JANITOR_SPEED);
            if (gameState->moveX != 0 && gameState->moveY != 0) moveSpeed *= 0.7071f; 
            gameState->x += static_cast<int>(gameState->moveX * moveSpeed); gameState->y += static_cast<int>(gameState->moveY * moveSpeed);
            SDL_Rect janitorRect = {gameState->x, gameState->y, drawnSpriteSize, drawnSpriteSize};
            for (int i = 0; i < MAX_POOPS; i++) {
                if (gameState->poops[i].active) {
                    SDL_Rect poopRect = {gameState->poops[i].x, gameState->poops[i].y, drawnSpriteSize, drawnSpriteSize};
                    if (SDL_HasIntersection(&janitorRect, &poopRect)) { gameState->cleanTimer = CLEAN_TIME; gameState->targetPoopIndex = i; break; }
                }
            }
        }
        if (gameState->x < 0) gameState->x = 0; if (gameState->y < 0) gameState->y = 0;
        if (gameState->x > screenWidth - drawnSpriteSize) gameState->x = screenWidth - drawnSpriteSize;
        if (gameState->y > FIELD_HEIGHT - drawnSpriteSize) gameState->y = FIELD_HEIGHT - drawnSpriteSize;
        if (isJanitorMoving && gameState->walkingChannel == -1) gameState->walkingChannel = Mix_PlayChannel(-1, walkingSound, -1);
        else if (!isJanitorMoving && gameState->walkingChannel != -1) { Mix_HaltChannel(gameState->walkingChannel); gameState->walkingChannel = -1; }
        for (int i = 0; i < gameState->animalCount; i++) {
            if (gameState->animals[i].type == creatureCat) {
                SDL_Rect janitorRect = {gameState->x, gameState->y, drawnSpriteSize, drawnSpriteSize};
                SDL_Rect animalRect = {gameState->animals[i].x, gameState->animals[i].y, drawnSpriteSize, drawnSpriteSize};
                if (SDL_HasIntersection(&janitorRect, &animalRect)) {
                    if (SDL_abs(gameState->animals[i].dx) != 4 && SDL_abs(gameState->animals[i].dy) != 4) {
                        Mix_PlayChannel(-1, catSound, 0); gameState->animals[i].moveTimer = rand() % 60 + 30;
                        int action = rand() % 4 + 1; gameState->animals[i].dx = 0; gameState->animals[i].dy = 0;
                        if (action == 1) gameState->animals[i].dy = -4; if (action == 2) gameState->animals[i].dy = 4;
                        if (action == 3) gameState->animals[i].dx = -4; if (action == 4) gameState->animals[i].dx = 4;
                    }
                }
            }
            if (gameState->animals[i].type == creatureDog) {
                SDL_Rect janitorRect = {gameState->x, gameState->y, drawnSpriteSize, drawnSpriteSize};
                SDL_Rect animalRect = {gameState->animals[i].x, gameState->animals[i].y, drawnSpriteSize, drawnSpriteSize};
                if (SDL_HasIntersection(&janitorRect, &animalRect)) {
                    bool isBrave = false;
                    if (gameState->cleanTimer > 0) isBrave = true;
                    else {
                        for (int k = 0; k < MAX_POOPS; k++) {
                            if (gameState->poops[k].active && gameState->poops[k].lifeTimer > gameState->POOP_LIFETIME * 0.75) {
                                int distSq = (gameState->x - gameState->poops[k].x) * (gameState->x - gameState->poops[k].x) + (gameState->y - gameState->poops[k].y) * (gameState->y - gameState->poops[k].y);
                                if (distSq < JANITOR_BRAVE_RADIUS * JANITOR_BRAVE_RADIUS) { isBrave = true; break; }
                            }
                        }
                    }
                    if (gameState->fearTimer == 0 && !isBrave) {
                        gameState->fearTimer = FEAR_TIME; gameState->cleanTimer = 0; gameState->targetPoopIndex = -1;
                        Mix_PlayChannel(-1, afraidSound, 0);
                        int dx = gameState->x - gameState->animals[i].x; int dy = gameState->y - gameState->animals[i].y;
                        gameState->forcedMoveX = (dx > 0) ? 1 : ((dx < 0) ? -1 : 0); gameState->forcedMoveY = (dy > 0) ? 1 : ((dy < 0) ? -1 : 0);
                        if (gameState->forcedMoveX == 0 && gameState->forcedMoveY == 0) gameState->forcedMoveX = 1;
                    }
                }
            }
            if (gameState->animals[i].moveTimer > 0) gameState->animals[i].moveTimer--;
            else {
                gameState->animals[i].moveTimer = rand() % 60 + 30; int action = rand() % 5;
                if (action == 0 && (rand() % 2 == 0)) {
                    bool placeOccupied = false;
                    SDL_Rect animalRect = {gameState->animals[i].x, gameState->animals[i].y, drawnSpriteSize, drawnSpriteSize};
                    for (int k = 0; k < MAX_POOPS; k++) {
                        if (gameState->poops[k].active) {
                            SDL_Rect poopRect = {gameState->poops[k].x, gameState->poops[k].y, drawnSpriteSize, drawnSpriteSize};
                            if (SDL_HasIntersection(&animalRect, &poopRect)) { placeOccupied = true; break; }
                        }
                    }
                    if (!placeOccupied) {
                        for (int p = 0; p < MAX_POOPS; p++) {
                            if (!gameState->poops[p].active) {
                                gameState->poops[p].active = true;
                                gameState->poops[p].x = gameState->animals[i].x; gameState->poops[p].y = gameState->animals[i].y;
                                gameState->poops[p].lifeTimer = 0; gameState->poops[p].hasFlies = false; gameState->poops[p].penaltyApplied = false;
                                Mix_PlayChannel(-1, pooSound, 0); break;
                            }
                        }
                    }
                }
                gameState->animals[i].dx = 0; gameState->animals[i].dy = 0;
                if (action == 1) gameState->animals[i].dy = -1; if (action == 2) gameState->animals[i].dy = 1;
                if (action == 3) gameState->animals[i].dx = -1; if (action == 4) gameState->animals[i].dx = 1;
            }
            gameState->animals[i].x += gameState->animals[i].dx; gameState->animals[i].y += gameState->animals[i].dy;
            if (gameState->animals[i].x < 0) { gameState->animals[i].x = 0; gameState->animals[i].moveTimer = 0; }
            if (gameState->animals[i].y < 0) { gameState->animals[i].y = 0; gameState->animals[i].moveTimer = 0; }
            if (gameState->animals[i].x > screenWidth - drawnSpriteSize) { gameState->animals[i].x = screenWidth - drawnSpriteSize; gameState->animals[i].moveTimer = 0; }
            if (gameState->animals[i].y > FIELD_HEIGHT - drawnSpriteSize) { gameState->animals[i].y = FIELD_HEIGHT - drawnSpriteSize; gameState->animals[i].moveTimer = 0; }
            if (gameState->animals[i].dy < 0) gameState->animals[i].direction = dUp; else if (gameState->animals[i].dy > 0) gameState->animals[i].direction = dDown;
            else if (gameState->animals[i].dx < 0) gameState->animals[i].direction = dLeft; else if (gameState->animals[i].dx > 0) gameState->animals[i].direction = dRight;
        }
        for (int i = 0; i < MAX_POOPS; i++) {
            if (gameState->poops[i].active) {
                if (gameState->cleanTimer == 0) gameState->poops[i].lifeTimer++;
                if (gameState->poops[i].lifeTimer > gameState->POOP_LIFETIME) {
                    gameState->poops[i].hasFlies = true;
                    if (!gameState->poops[i].penaltyApplied) { gameState->lives--; gameState->poops[i].penaltyApplied = true; Mix_PlayChannel(-1, lostLiveSound, 0); }
                }
            }
        }
        if (start - gameState->lastAnimTime > FRAME_DURATION) { gameState->animTick = (gameState->animTick + 1) % 4; gameState->lastAnimTime = start; }
    } 
    
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255); SDL_RenderClear(renderer);
    Uint8 bgR = (Uint8)std::min(255, 60 + (gameState->level * 10));
    Uint8 bgG = (Uint8)std::max(40, 120 - (gameState->level * 5));
    Uint8 bgB = (Uint8)std::max(40, 60 - (gameState->level * 2));
    if (backgroundTexture) {
        SDL_SetTextureColorMod(backgroundTexture, bgR + 50, bgG + 50, bgB + 50);
        for (int y = 0; y < FIELD_HEIGHT; y += drawnSpriteSize) {
            for (int x = 0; x < screenWidth; x += drawnSpriteSize) {
                SDL_Rect destRect = { x, y + HUD_HEIGHT, drawnSpriteSize, drawnSpriteSize };
                SDL_RenderCopy(renderer, backgroundTexture, NULL, &destRect);
            }
        }
    } else {
        SDL_SetRenderDrawColor(renderer, bgR, bgG, bgB, 255);
        SDL_Rect fieldRect = {0, HUD_HEIGHT, screenWidth, FIELD_HEIGHT};
        SDL_RenderFillRect(renderer, &fieldRect);
    }
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255); SDL_Rect hudRect = {0, 0, screenWidth, HUD_HEIGHT}; SDL_RenderFillRect(renderer, &hudRect);
    for (int i = 0; i < gameState->lives; i++) drawHeart(renderer, 10.0f + i * 25.0f, 5.0f, 20.0f);
    drawNumber(gameState->coins, 100, 0, 30, numbersTexture);
    SDL_SetRenderDrawColor(renderer, 50, 100, 255, 255);
    float levelW = 6.0f; float levelStep = 8.0f;
    for (int i = 0; i < gameState->level; i++) {
        SDL_Rect lvlRect = {(int)(screenWidth - 15.0f - i * levelStep), 5, (int)levelW, 20};
        SDL_RenderFillRect(renderer, &lvlRect);
    }
    SDL_SetRenderDrawColor(renderer, 50, 200, 50, 255);
    float poopW = 4.0f; float poopStep = 6.0f; float iconSize = 20.0f; float padding = 5.0f;
    float totalWidth = iconSize + padding + (gameState->poopsToNextLevel * poopStep); float startX = (screenWidth - totalWidth) / 2.0f;
    SDL_Rect iconSrc = { objectPoop * 4 * spriteSize, 0, spriteSize, spriteSize };
    SDL_Rect iconDst = { (int)startX, (int)((HUD_HEIGHT - iconSize) / 2.0f), (int)iconSize, (int)iconSize };
    SDL_RenderCopy(renderer, texture, &iconSrc, &iconDst);
    for (int i = 0; i < gameState->poopsToNextLevel; i++) {
        SDL_Rect poopRect = {(int)(startX + iconSize + padding + i * poopStep), 5, (int)poopW, 20};
        SDL_RenderFillRect(renderer, &poopRect);
    }
    if (gameState->lives != gameState->lastLives || gameState->coins != gameState->lastScore || gameState->level != gameState->lastLevel || gameState->poopsToNextLevel != gameState->lastPoopsToNextLevel) {
        std::string title = "JanitorGame | Level: " + std::to_string(gameState->level) + " | Score: " + std::to_string(gameState->coins) + " | Lives: " + std::to_string(gameState->lives) + " | Left: " + std::to_string(gameState->poopsToNextLevel);
        SDL_SetWindowTitle(window, title.c_str());
        gameState->lastLives = gameState->lives; gameState->lastScore = gameState->coins; gameState->lastLevel = gameState->level; gameState->lastPoopsToNextLevel = gameState->lastPoopsToNextLevel;
    }
    for (int i = 0; i < MAX_POOPS; i++) {
        if (gameState->poops[i].active) {
            bool shouldDraw = true; int directionToDraw = 0;
            if (gameState->poops[i].hasFlies) { if ((SDL_GetTicks() / 100) % 2 == 0) directionToDraw = 1; }
            else { if (gameState->poops[i].lifeTimer > gameState->POOP_LIFETIME * 0.75) { if ((SDL_GetTicks() / 200) % 2 == 0) shouldDraw = false; } }
            if (shouldDraw) drawCreature(objectPoop, 0, directionToDraw, gameState->poops[i].x, gameState->poops[i].y);
        }
    }
    for (int i = 0; i < gameState->animalCount; i++) drawCreature(gameState->animals[i].type, gameState->animTick, gameState->animals[i].direction, gameState->animals[i].x, gameState->animals[i].y);
    drawCreature(creatureJanitor, gameState->animTick, gameState->direction, gameState->x, gameState->y);
    drawJoystick();
    if (gameState->levelUpTimer > 0) {
        if (gameState->levelUpTimer > 70) {
            int alpha = (gameState->levelUpTimer - 70) * 12; if (alpha > 255) alpha = 255;
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 255, 255, 200, (Uint8)alpha);
            SDL_Rect screenRect = {0, HUD_HEIGHT, screenWidth, FIELD_HEIGHT};
            SDL_RenderFillRect(renderer, &screenRect);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        }
    }
    if (gameState->isPaused) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 150);
        SDL_Rect pauseOverlayRect = {0, 0, screenWidth, WINDOW_HEIGHT};
        SDL_RenderFillRect(renderer, &pauseOverlayRect);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    }
    SDL_RenderPresent(renderer);
}

void loop_game_over() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_KEYDOWN || event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_FINGERDOWN || event.type == SDL_QUIT) {
            gameState->currentState = STATE_HIGH_SCORES;
        } else if (event.type == SDL_CONTROLLERDEVICEADDED) {
            if (!gameState->gamepad) {
                gameState->gamepad = SDL_GameControllerOpen(event.cdevice.which);
            }
        } else if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
            if (gameState->gamepad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gameState->gamepad)) == event.cdevice.which) {
                SDL_GameControllerClose(gameState->gamepad);
                gameState->gamepad = nullptr;
            }
        }
    }
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, gameOverTexture, NULL, NULL);
    drawNumber(gameState->level, 470, 265, 77, numbersTexture);
    drawNumber(gameState->coins, 470, 385, 77, numbersTexture);
    SDL_RenderPresent(renderer);
}

void loop_high_scores() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            gameState->isRunning = false;
        } else if (event.type == SDL_KEYDOWN || event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_FINGERDOWN) {
#ifdef __EMSCRIPTEN__
            reset_game_state();
            gameState->currentState = STATE_BEGIN_SCREEN;
#else
            gameState->isRunning = false;
#endif
        } else if (event.type == SDL_CONTROLLERDEVICEADDED) {
            if (!gameState->gamepad) {
                gameState->gamepad = SDL_GameControllerOpen(event.cdevice.which);
            }
        } else if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
            if (gameState->gamepad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gameState->gamepad)) == event.cdevice.which) {
                SDL_GameControllerClose(gameState->gamepad);
                gameState->gamepad = nullptr;
            }
        }
    }
    SDL_RenderClear(renderer);
    if(scoreTableTexture) SDL_RenderCopy(renderer, scoreTableTexture, NULL, NULL);
    for (size_t i = 0; i < highScores.size(); ++i) {
        drawNumber(i + 1, 450, 100 + i * 90, 70, numbersTexture);
        drawNumber(highScores[i], 650, 100 + i * 90, 70, numbersTexture);
    }
    SDL_RenderPresent(renderer);
}

void master_loop() {
    Uint32 frameStart = SDL_GetTicks();
    if (!gameState->isRunning) gameState->currentState = STATE_EXIT;
    switch(gameState->currentState) {
        case STATE_INIT: gameState->currentState = STATE_BEGIN_SCREEN; break;
        case STATE_INTRO: loop_intro(); break;
        case STATE_BEGIN_SCREEN: loop_begin_screen(); break;
        case STATE_DIFFICULTY_SELECT: loop_difficulty_select(); break;
        case STATE_GAMEPLAY: loop_gameplay(); break;
        case STATE_GAME_OVER:
            if (gameState->walkingChannel != -1) { Mix_HaltChannel(gameState->walkingChannel); gameState->walkingChannel = -1; }
            if (!gameState->scoresUpdated) {
                updateHighScores(gameState->coins);
                gameState->scoresUpdated = true;
            }
            loop_game_over();
            break;
        case STATE_HIGH_SCORES: loop_high_scores(); break;
        case STATE_EXIT:
            #ifdef __EMSCRIPTEN__
            emscripten_force_exit(0);
            #else
            gameState->isRunning = false;
            #endif
            break;
    }
    Uint32 frameTime = SDL_GetTicks() - frameStart;
    if (frameTime < FRAME_TIME) SDL_Delay(FRAME_TIME - frameTime);
}

int main(int argc, char* argv[])
{
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    
    gameState = new GameState();
    reset_game_state();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) { return 1; }
    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) < 0) { return 1; }

    Uint32 windowFlags = 0;
#ifdef __EMSCRIPTEN__
    windowFlags = SDL_WINDOW_RESIZABLE;
#else
    windowFlags = SDL_WINDOW_FULLSCREEN;
#endif

    window = SDL_CreateWindow("JanitorGame", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, screenWidth, WINDOW_HEIGHT, windowFlags);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    SDL_RenderSetLogicalSize(renderer, screenWidth, WINDOW_HEIGHT);

#ifdef __EMSCRIPTEN__
    // Для веб-сборки монтируем постоянную файловую систему (IDBFS) для сохранения рекордов.
    // Затем мы синхронизируем ее, и как только она будет готова, вызываем нашу C++ функцию loadHighScores.
    EM_ASM(
        FS.mkdir('/gamedata');
        FS.mount(IDBFS, {}, '/gamedata');
        FS.syncfs(true, function (err) {
            if (err) { console.error('Error syncing filesystem:', err); }
            else { console.log('Initial filesystem sync done.'); }
            // После синхронизации вызываем C++ функцию для загрузки рекордов из виртуальной файловой системы.
            Module.ccall('loadHighScores', 'void', [], []);
        });

        // This event listener ensures that the in-memory filesystem is saved to persistent storage (IndexedDB)
        // right before the user closes or reloads the page.
        window.addEventListener('beforeunload', function (e) {
            // Inside a 'beforeunload' handler, FS.syncfs becomes a synchronous, blocking operation,
            // forcing the browser to wait until the save is complete.
            FS.syncfs(false, function (err) {
                if (err) { console.error('Failed to save filesystem on page close:', err); }
            });
        });
    );
#else
    // Для десктопной сборки загружаем рекорды сразу.
    loadHighScores();
#endif

    pooSound = Mix_LoadWAV("POO.WAV"); sweepSound = Mix_LoadWAV("sweep.wav"); afraidSound = Mix_LoadWAV("afraid.wav");
    catSound = Mix_LoadWAV("cat.wav"); walkingSound = Mix_LoadWAV("walking.wav"); lostLiveSound = Mix_LoadWAV("lostlive.wav");
    levelUpSound = Mix_LoadWAV("levelup.wav");

    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) { if ((gameState->gamepad = SDL_GameControllerOpen(i))) break; }
    }

    SDL_Surface* surface = SDL_LoadBMP("sprites.bmp");
    SDL_SetColorKey(surface, SDL_TRUE, SDL_MapRGB(surface->format, 0, 0, 0));
    texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);

    SDL_Surface* beginSurface = SDL_LoadBMP("begin.bmp");
    beginTexture = SDL_CreateTextureFromSurface(renderer, beginSurface);
    SDL_FreeSurface(beginSurface);
    
    gameState->titleLastAnimTime = SDL_GetTicks();
    
    SDL_Surface* numbersSurface = SDL_LoadBMP("numbers.bmp");
    SDL_Surface* formattedSurface = SDL_ConvertSurfaceFormat(numbersSurface, SDL_PIXELFORMAT_RGBA8888, 0);
    SDL_FreeSurface(numbersSurface);
    SDL_LockSurface(formattedSurface);
    Uint32* pixels = (Uint32*)formattedSurface->pixels;
    int pixelCount = (formattedSurface->pitch / 4) * formattedSurface->h;
    for (int i = 0; i < pixelCount; ++i) {
        Uint8 r, g, b, a;
        SDL_GetRGBA(pixels[i], formattedSurface->format, &r, &g, &b, &a);
        if (r == 0 && g == 0 && b == 0) { a = 0; } else { a = 255; }
        pixels[i] = SDL_MapRGBA(formattedSurface->format, r, g, b, a);
    }
    SDL_UnlockSurface(formattedSurface);
    numbersTexture = SDL_CreateTextureFromSurface(renderer, formattedSurface);
    SDL_SetTextureBlendMode(numbersTexture, SDL_BLENDMODE_BLEND);
    SDL_FreeSurface(formattedSurface);
    
    SDL_Surface* diffSurf=SDL_LoadBMP("dif.bmp");
    diffTexture = SDL_CreateTextureFromSurface(renderer, diffSurf); 
    SDL_FreeSurface(diffSurf);

    SDL_Surface* goSurf = SDL_LoadBMP("gameover.bmp");
    gameOverTexture = SDL_CreateTextureFromSurface(renderer, goSurf);
    SDL_FreeSurface(goSurf);

    SDL_Surface* scoreSurf = SDL_LoadBMP("scoretable.bmp");
    scoreTableTexture = SDL_CreateTextureFromSurface(renderer, scoreSurf);
    SDL_FreeSurface(scoreSurf);

    SDL_Surface* bgSurface = SDL_LoadBMP("ground.bmp");
    if (bgSurface) {
        backgroundTexture = SDL_CreateTextureFromSurface(renderer, bgSurface);
        SDL_FreeSurface(bgSurface);
        SDL_Log("Successfully loaded ground.bmp");
    } else {
        SDL_Log("Failed to load ground.bmp: %s", SDL_GetError());
    }

    gameState->currentState = STATE_BEGIN_SCREEN;

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(master_loop, 0, 1);
#else
    while (gameState->isRunning) {
        master_loop();
    }
#endif

    SDL_DestroyTexture(texture); SDL_DestroyTexture(backgroundTexture); SDL_DestroyTexture(numbersTexture);
    SDL_DestroyTexture(beginTexture); SDL_DestroyTexture(diffTexture); SDL_DestroyTexture(gameOverTexture); SDL_DestroyTexture(scoreTableTexture);
    Mix_FreeChunk(pooSound); Mix_FreeChunk(sweepSound); Mix_FreeChunk(afraidSound);
    Mix_FreeChunk(catSound); Mix_FreeChunk(walkingSound); Mix_FreeChunk(lostLiveSound); Mix_FreeChunk(levelUpSound);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_GameControllerClose(gameState->gamepad);
    Mix_CloseAudio();
    SDL_Quit();
    delete gameState;
    return 0;
}

void reset_game_state() {
    gameState->lives = 3;
    gameState->coins = 0;
    gameState->cleanPoops = 0;
    gameState->level = 1;
    gameState->poopsToNextLevel = 5;
    gameState->animalCount = 4;
    gameState->x = 100;
    gameState->y = 100;
    gameState->direction = dDown;
    gameState->gameWon = false;
    gameState->isPaused = false;
    gameState->scoresUpdated = false;
#ifndef __EMSCRIPTEN__
    gameState->audioContextResumed = true; // На десктопе аудио работает сразу
#else
    gameState->audioContextResumed = false; // В вебе ждем клика
#endif
    for (int i=0;i<MAXANIMALS;i++) {
        bool safePos = false;
        while (!safePos) {
            gameState->animals[i].x = rand() % (screenWidth - drawnSpriteSize);
            gameState->animals[i].y = rand() % (FIELD_HEIGHT - drawnSpriteSize);
            int dx = gameState->animals[i].x - gameState->x;
            int dy = gameState->animals[i].y - gameState->y;
            if ((dx * dx + dy * dy) > (250 * 250)) safePos = true;
        }
        gameState->animals[i].dx = 0; gameState->animals[i].dy = 0;
        gameState->animals[i].moveTimer = 0;
        gameState->animals[i].type = rand() % 2 +1;
        gameState->animals[i].direction = dDown;
    }
    for(int i=0; i<MAX_POOPS; i++) {
        gameState->poops[i].active = false;
        gameState->poops[i].lifeTimer = 0;
        gameState->poops[i].hasFlies = false;
        gameState->poops[i].penaltyApplied = false;
    }

    for (int i = 0; i < 4; i++) {
        gameState->titleAnimals[i].x = rand() % (screenWidth - drawnSpriteSize); gameState->titleAnimals[i].y = rand() % (FIELD_HEIGHT - drawnSpriteSize);
        gameState->titleAnimals[i].dx = 0; gameState->titleAnimals[i].dy = 0;
        gameState->titleAnimals[i].moveTimer = rand() % 60 + 30; gameState->titleAnimals[i].type = (i < 2) ? creatureCat : creatureDog;
        gameState->titleAnimals[i].direction = dDown;
    }
    gameState->titleLastAnimTime = SDL_GetTicks();
}