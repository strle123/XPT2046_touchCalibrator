#include <SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <regex>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
// ...existing code up to get_exe_dir()...
static std::string get_exe_dir() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf)-1);
    if (n <= 0) return std::string();
    buf[n] = '\0';
    std::string path(buf);
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) return std::string();
    return path.substr(0, pos);
}

static std::string find_config_path() {
    // ...existing code...
    const char* envPath = getenv("TOUCH_CONFIG_PATH");
    if (envPath && *envPath) {
        std::ifstream f(envPath);
        if (f.good()) return std::string(envPath);
    }
    std::vector<std::string> candidates;
    candidates.push_back("/etc/xpt2046/touch_config.txt");
    candidates.push_back("installation/touch_config.txt");
    candidates.push_back("touch_config.txt");
    std::string exeDir = get_exe_dir();
    if (!exeDir.empty()) {
        candidates.push_back(exeDir + "/installation/touch_config.txt");
        candidates.push_back(exeDir + "/../installation/touch_config.txt");
    }
    for (const auto& p : candidates) {
        std::ifstream f(p);
        if (f.good()) return p;
    }
    return std::string();
}

static void load_ranges(const std::string& cfgPath, int& min_x, int& max_x, int& min_y, int& max_y) {
    // ...existing code...
    min_x = 0; max_x = 4095; min_y = 0; max_y = 4095;
    if (cfgPath.empty()) return;
    std::ifstream in(cfgPath);
    if (!in.good()) return;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        std::string key = line.substr(0, pos);
        std::string val = line.substr(pos + 1);
        auto ltrim = [](std::string& s){ s.erase(0, s.find_first_not_of(" \t")); };
        auto rtrim = [](std::string& s){ s.erase(s.find_last_not_of(" \t") + 1); };
        ltrim(key); rtrim(key); ltrim(val); rtrim(val);
        int iv = std::atoi(val.c_str());
        if (key == "min_x") min_x = iv;
        else if (key == "max_x") max_x = iv;
        else if (key == "min_y") min_y = iv;
        else if (key == "max_y") max_y = iv;
    }
    if (max_x <= min_x) max_x = min_x + 1;
    if (max_y <= min_y) max_y = min_y + 1;
}

static std::string find_calibrator_binary() {
    // ...existing code...
    std::vector<std::string> candidates = {
        get_exe_dir() + "/xpt2046_calibrator",
        get_exe_dir() + "/../build/xpt2046_calibrator",
        "xpt2046_calibrator"
    };
    for (const auto& c : candidates) {
        std::ifstream f(c);
        if (f.good()) return c;
    }
    return std::string();
}

struct UIState {
    bool toggled = false;
    bool touch_present = false;
    int cx = 400, cy = 240;
    int last_touch_x = 400, last_touch_y = 240;
    int raw_x = 0, raw_y = 0;
    float slider_value = 0.5f;
    bool dragging_slider = false;
};

enum class IdleCursorMode {
    ShowRaw = 0,   // show pointer even when not touching (may drift due to idle noise)
    FreezeLast = 1, // keep last touch position when not touching
    Hide = 2       // do not draw pointer when not touching
};

static IdleCursorMode parse_idle_cursor_mode(const char* s) {
    if (!s || !*s) return IdleCursorMode::FreezeLast;
    if (std::strcmp(s, "show") == 0 || std::strcmp(s, "raw") == 0) return IdleCursorMode::ShowRaw;
    if (std::strcmp(s, "freeze") == 0 || std::strcmp(s, "last") == 0) return IdleCursorMode::FreezeLast;
    if (std::strcmp(s, "hide") == 0 || std::strcmp(s, "off") == 0) return IdleCursorMode::Hide;
    return IdleCursorMode::FreezeLast;
}

static const char* idle_cursor_mode_name(IdleCursorMode m) {
    switch (m) {
        case IdleCursorMode::ShowRaw: return "show";
        case IdleCursorMode::FreezeLast: return "freeze";
        case IdleCursorMode::Hide: return "hide";
    }
    return "freeze";
}

static void draw_button(SDL_Renderer* r, SDL_Rect rect, bool active, SDL_Color base, SDL_Color border) {
    SDL_SetRenderDrawColor(r, active ? 48 : base.r, active ? 173 : base.g, active ? 86 : base.b, 255);
    SDL_RenderFillRect(r, &rect);
    SDL_SetRenderDrawColor(r, border.r, border.g, border.b, 255);
    SDL_RenderDrawRect(r, &rect);
}

static void draw_slider(SDL_Renderer* r, SDL_Rect track, float value, SDL_Color base, SDL_Color border, SDL_Color knob) {
    SDL_SetRenderDrawColor(r, base.r, base.g, base.b, 255);
    SDL_RenderFillRect(r, &track);
    SDL_SetRenderDrawColor(r, border.r, border.g, border.b, 255);
    SDL_RenderDrawRect(r, &track);
    int knobW = 18;
    int knobX = track.x + (int)(value * (track.w - knobW));
    SDL_Rect k{knobX, track.y - 6, knobW, track.h + 12};
    SDL_SetRenderDrawColor(r, knob.r, knob.g, knob.b, 255);
    SDL_RenderFillRect(r, &k);
    SDL_SetRenderDrawColor(r, border.r, border.g, border.b, 255);
    SDL_RenderDrawRect(r, &k);
}

// Only one definition of draw_pointer should exist, outside main.
static void draw_pointer(SDL_Renderer* r, int x, int y, SDL_Color col, SDL_Color border) {
    SDL_Rect dot{x-6, y-6, 12, 12};
    SDL_SetRenderDrawColor(r, col.r, col.g, col.b, 255);
    SDL_RenderFillRect(r, &dot);
    SDL_SetRenderDrawColor(r, border.r, border.g, border.b, 255);
    SDL_RenderDrawRect(r, &dot);
}

int main(int argc, char** argv) {
    // ...existing code up to width/height...
    int width = 800, height = 480;
    // Declare all variables at the top to avoid jump-to-case errors
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    // Colors are defined once in main (after renderer is created)

    // Video driver selection via environment (X11/kmsdrm/fbcon)
    const char* disp = getenv("DISPLAY");
    if (disp && *disp) setenv("SDL_VIDEO_X11_XSHM", "0", 0);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "[ERROR] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    win = SDL_CreateWindow("XPT2046 GUI Test (SDL2)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, 0);
    if (!win) {
        std::fprintf(stderr, "[ERROR] Failed to create window: %s\n", SDL_GetError());
        std::fprintf(stderr, "Hints: Set SDL_VIDEODRIVER=kmsdrm or fbcon when running on console.\n");
        SDL_Quit();
        return 1;
    }
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) {
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!ren) {
        std::fprintf(stderr, "[ERROR] Failed to create renderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    // Colors
    SDL_Color WHITE{255, 255, 255, 255};
    SDL_Color BLACK{0, 0, 0, 255};
    SDL_Color GRAY{180, 180, 180, 255};
    SDL_Color BLUE{66, 135, 245, 255};
    SDL_Color GREEN{48, 173, 86, 255};
    SDL_Color RED{220, 68, 68, 255};
    SDL_Color ORANGE{245, 161, 66, 255};




    // UI layout: slider at top, buttons centered
    SDL_Rect slider{40, 120, 720, 24};
    int btnW = 160, btnH = 70;
    int spacing = 40;
    int totalW = btnW * 3 + spacing * 2;
    int startX = (width - totalW) / 2;
    int btnY = height / 2 - btnH / 2;
    SDL_Rect btn1{startX, btnY, btnW, btnH};
    SDL_Rect btn2{startX + btnW + spacing, btnY, btnW, btnH};
    SDL_Rect btnExt{startX + 2 * (btnW + spacing), btnY, btnW, btnH};

    // Small edge buttons for min/max X/Y test (3 per edge)
    int edgeBtnSize = 24;
    int edgeBtnPad = 8;
    SDL_Rect edgeBtns[12];
    // Top edge
    for (int i = 0; i < 3; ++i) {
        edgeBtns[i].x = 60 + i*(width-120)/2;
        edgeBtns[i].y = edgeBtnPad;
        edgeBtns[i].w = edgeBtnSize;
        edgeBtns[i].h = edgeBtnSize;
    }
    // Bottom edge
    for (int i = 0; i < 3; ++i) {
        edgeBtns[3+i].x = 60 + i*(width-120)/2;
        edgeBtns[3+i].y = height-edgeBtnPad-edgeBtnSize;
        edgeBtns[3+i].w = edgeBtnSize;
        edgeBtns[3+i].h = edgeBtnSize;
    }
    // Left edge
    for (int i = 0; i < 3; ++i) {
        edgeBtns[6+i].x = edgeBtnPad;
        edgeBtns[6+i].y = 60 + i*(height-120)/2;
        edgeBtns[6+i].w = edgeBtnSize;
        edgeBtns[6+i].h = edgeBtnSize;
    }
    // Right edge
    for (int i = 0; i < 3; ++i) {
        edgeBtns[9+i].x = width-edgeBtnPad-edgeBtnSize;
        edgeBtns[9+i].y = 60 + i*(height-120)/2;
        edgeBtns[9+i].w = edgeBtnSize;
        edgeBtns[9+i].h = edgeBtnSize;
    }

    // Simple function to draw crude text using lines (for BTN 1, BTN 2, EXT)
    auto draw_text = [&](SDL_Renderer* r, int x, int y, const char* txt) {
        SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
        const int charSpacing = 18;
        for (int i = 0; txt[i]; ++i) {
            int bx = x + i * charSpacing;
            switch (txt[i]) {
                case 'B':
                    SDL_RenderDrawLine(r, bx+2, y+2, bx+2, y+18);
                    SDL_RenderDrawLine(r, bx+2, y+2, bx+12, y+2);
                    SDL_RenderDrawLine(r, bx+2, y+10, bx+12, y+10);
                    SDL_RenderDrawLine(r, bx+2, y+18, bx+12, y+18);
                    SDL_RenderDrawLine(r, bx+12, y+2, bx+12, y+10);
                    SDL_RenderDrawLine(r, bx+12, y+10, bx+12, y+18);
                    break;
                case 'T':
                    SDL_RenderDrawLine(r, bx+2, y+2, bx+14, y+2);
                    SDL_RenderDrawLine(r, bx+8, y+2, bx+8, y+18);
                    break;
                case 'N':
                    SDL_RenderDrawLine(r, bx+2, y+18, bx+2, y+2);
                    SDL_RenderDrawLine(r, bx+2, y+2, bx+14, y+18);
                    SDL_RenderDrawLine(r, bx+14, y+2, bx+14, y+18);
                    break;
                case '1':
                    SDL_RenderDrawLine(r, bx+8, y+2, bx+8, y+18);
                    SDL_RenderDrawLine(r, bx+6, y+4, bx+8, y+2);
                    SDL_RenderDrawLine(r, bx+6, y+18, bx+10, y+18);
                    break;
                case '2':
                    SDL_RenderDrawLine(r, bx+4, y+4, bx+12, y+4);
                    SDL_RenderDrawLine(r, bx+12, y+4, bx+12, y+10);
                    SDL_RenderDrawLine(r, bx+4, y+10, bx+12, y+10);
                    SDL_RenderDrawLine(r, bx+4, y+10, bx+4, y+16);
                    SDL_RenderDrawLine(r, bx+4, y+16, bx+12, y+16);
                    break;
                case 'E':
                    SDL_RenderDrawLine(r, bx+12, y+2, bx+4, y+2);
                    SDL_RenderDrawLine(r, bx+4, y+2, bx+4, y+18);
                    SDL_RenderDrawLine(r, bx+4, y+10, bx+10, y+10);
                    SDL_RenderDrawLine(r, bx+4, y+18, bx+12, y+18);
                    break;
                case 'X':
                    SDL_RenderDrawLine(r, bx+4, y+2, bx+12, y+18);
                    SDL_RenderDrawLine(r, bx+12, y+2, bx+4, y+18);
                    break;
                default:
                    break;
            }
        }
    };

    // Load config ranges
    std::string cfg = find_config_path();
    int min_x, max_x, min_y, max_y;
    load_ranges(cfg, min_x, max_x, min_y, max_y);
    // Allow environment overrides for live testing (from calibrate.sh)
    if (const char* v = getenv("XPT_MIN_X")) { if (*v) min_x = std::atoi(v); }
    if (const char* v = getenv("XPT_MAX_X")) { if (*v) max_x = std::atoi(v); }
    if (const char* v = getenv("XPT_MIN_Y")) { if (*v) min_y = std::atoi(v); }
    if (const char* v = getenv("XPT_MAX_Y")) { if (*v) max_y = std::atoi(v); }
    // Use ranges directly; calibrator applies swap/invert before clamping

    // Start calibrator process
    std::string calibrator = find_calibrator_binary();
    if (calibrator.empty()) {
        std::fprintf(stderr, "[ERROR] xpt2046_calibrator binary not found. Build it first.\n");
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        std::fprintf(stderr, "[ERROR] pipe() failed\n");
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    pid_t pid = fork();
    if (pid == 0) {
        // Child: exec calibrator, redirect stdout to pipe
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        setenv("CALIBRATION_RUNNING", "1", 1);
        if (!cfg.empty()) setenv("TOUCH_CONFIG_PATH", cfg.c_str(), 1);
        // Pass optional invert/swap from environment (set by calibrate.sh)
        const char* ix = getenv("XPT_INVERT_X");
        const char* iy = getenv("XPT_INVERT_Y");
        const char* sw = getenv("XPT_SWAP_XY");
        std::vector<const char*> args;
        args.push_back(calibrator.c_str());
        if (ix && *ix) { args.push_back("--invert_x"); args.push_back(ix); }
        if (iy && *iy) { args.push_back("--invert_y"); args.push_back(iy); }
        if (sw && *sw) { args.push_back("--swap_xy"); args.push_back(sw); }
        args.push_back(nullptr);
        execv(calibrator.c_str(), const_cast<char* const*>(args.data()));
        std::perror("execv");
        _exit(127);
    }
    // Parent
    close(pipefd[1]);
    FILE* pipeIn = fdopen(pipefd[0], "r");

    // Non-blocking read lines from pipe
    int pipe_fd = fileno(pipeIn);
    int flags = fcntl(pipe_fd, F_GETFL, 0);
    fcntl(pipe_fd, F_SETFL, flags | O_NONBLOCK);
    char buf[1024];

    // Regex to parse driver output lines
    // Supports both formats:
    //  - legacy: X/Y are raw-axis units
    //  - new: includes SX/SY (screen coords)
    std::regex re(R"(^\[SPI\]\s+XPT2046\s+X:\s*(\d+)\s+Y:\s*(\d+)\s+\(raw X:\s*(\d+)\s+raw Y:\s*(\d+)(?:\s+SX:\s*(\d+)\s+SY:\s*(\d+))?(?:\s+Z:\s*(\d+))?(?:\s+DOWN:\s*(\d+))?.*\))");

    UIState st;
    IdleCursorMode idleMode = parse_idle_cursor_mode(getenv("XPT_GUI_IDLE_CURSOR"));
    std::fprintf(stderr, "[INFO] GUI idle cursor mode: %s (press 'i' to cycle)\n", idle_cursor_mode_name(idleMode));
    bool running = true;
    auto scale_to_screen = [&](int x, int y){
        int sx = (int)((x - min_x) * (width / (double)(max_x - min_x)));
        int sy = (int)((y - min_y) * (height / (double)(max_y - min_y)));
        if (sx < 0) sx = 0; if (sy < 0) sy = 0;
        if (sx >= width) sx = width-1; if (sy >= height) sy = height-1;
        return std::pair<int,int>(sx, sy);
    };

    while (running) {
        int pressedEdgeBtn = -1;
        // Pump SDL events
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_ESCAPE || k == SDLK_q) {
                    running = false;
                } else if (k == SDLK_i) {
                    if (idleMode == IdleCursorMode::ShowRaw) idleMode = IdleCursorMode::FreezeLast;
                    else if (idleMode == IdleCursorMode::FreezeLast) idleMode = IdleCursorMode::Hide;
                    else idleMode = IdleCursorMode::ShowRaw;
                    std::fprintf(stderr, "[INFO] GUI idle cursor mode: %s\n", idle_cursor_mode_name(idleMode));
                }
            }
        }
        // Read latest driver line(s)
        ssize_t n = read(pipe_fd, buf, sizeof(buf)-1);
        if (n > 0) {
            buf[n] = '\0';
            std::istringstream iss{std::string(buf)};
            std::string line;
            while (std::getline(iss, line)) {
                std::smatch m;
                if (std::regex_search(line, m, re)) {
                    int x = std::atoi(m[1].str().c_str());
                    int y = std::atoi(m[2].str().c_str());
                    int rx = std::atoi(m[3].str().c_str());
                    int ry = std::atoi(m[4].str().c_str());
                    int sx = (m.size() > 5 && m[5].matched) ? std::atoi(m[5].str().c_str()) : -1;
                    int sy = (m.size() > 6 && m[6].matched) ? std::atoi(m[6].str().c_str()) : -1;
                    int z = (m.size() > 7 && m[7].matched) ? std::atoi(m[7].str().c_str()) : -1;
                    int down = (m.size() > 8 && m[8].matched) ? std::atoi(m[8].str().c_str()) : -1;
                    st.raw_x = rx; st.raw_y = ry;

                    // Touch presence: prefer DOWN if present, else Z, else old heuristic
                    bool touch = false;
                    if (down == 0 || down == 1) touch = (down == 1);
                    else if (z >= 0) touch = (z > 0);
                    else touch = (rx >= 50 && rx <= 4045 && ry >= 50 && ry <= 4045);
                    st.touch_present = touch;

                    // Prefer SX/SY if present; otherwise scale X/Y using min/max
                    if (st.touch_present || idleMode == IdleCursorMode::ShowRaw) {
                        if (sx >= 0 && sy >= 0) {
                            if (sx < 0) sx = 0; if (sy < 0) sy = 0;
                            if (sx >= width) sx = width - 1;
                            if (sy >= height) sy = height - 1;
                            st.cx = sx; st.cy = sy;
                        } else {
                            auto p = scale_to_screen(x, y);
                            st.cx = p.first; st.cy = p.second;
                        }
                    }
                    if (st.touch_present) {
                        st.last_touch_x = st.cx;
                        st.last_touch_y = st.cy;
                    }

                    // UI interactions should only happen when touching.
                    if (st.touch_present) {
                        SDL_Point pt{st.cx, st.cy};
                        if (SDL_PointInRect(&pt, &btnExt)) running = false;
                        st.toggled = SDL_PointInRect(&pt, &btn2);
                        if (SDL_PointInRect(&pt, &slider)) {
                            st.dragging_slider = true;
                            int rel = st.cx - slider.x; if (rel < 0) rel = 0; if (rel > slider.w) rel = slider.w;
                            st.slider_value = rel / (float)slider.w;
                        } else {
                            st.dragging_slider = false;
                        }
                    } else {
                        st.toggled = false;
                        st.dragging_slider = false;
                    }
                }
            }
        }

        // Draw frame
        SDL_SetRenderDrawColor(ren, 250, 250, 250, 255);
        SDL_RenderClear(ren);
        SDL_Point ptDraw{st.cx, st.cy};

        // Draw edge buttons (3 per edge)
        SDL_Point ptTouch{st.cx, st.cy};
        for (int i = 0; i < 12; ++i) {
            bool inside = st.touch_present && SDL_PointInRect(&ptTouch, &edgeBtns[i]);
            if (inside) pressedEdgeBtn = i;
            SDL_SetRenderDrawColor(ren, inside ? 0 : 80, inside ? 200 : 80, inside ? 0 : 80, 255);
            SDL_RenderFillRect(ren, &edgeBtns[i]);
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderDrawRect(ren, &edgeBtns[i]);
        }
        if (pressedEdgeBtn != -1) {
            printf("[EDGE_BTN] Pressed edge button %d\n", pressedEdgeBtn);
            fflush(stdout);
        }
        // Draw slider at top
        draw_slider(ren, slider, st.slider_value, GRAY, BLACK, ORANGE);
        // Draw centered buttons
        draw_button(ren, btn1, st.touch_present && SDL_PointInRect(&ptDraw, &btn1), BLUE, BLACK);
        draw_button(ren, btn2, st.toggled, BLUE, BLACK);
        draw_button(ren, btnExt, st.touch_present && SDL_PointInRect(&ptDraw, &btnExt), RED, BLACK);
        // Draw button labels (crude text)
        draw_text(ren, btn1.x + btn1.w/2 - 32, btn1.y + btn1.h/2 - 10, "BTN 1");
        draw_text(ren, btn2.x + btn2.w/2 - 32, btn2.y + btn2.h/2 - 10, "BTN 2");
        draw_text(ren, btnExt.x + btnExt.w/2 - 24, btnExt.y + btnExt.h/2 - 10, "EXT");

        if (st.touch_present) {
            draw_pointer(ren, st.cx, st.cy, RED, BLACK);
        } else {
            if (idleMode == IdleCursorMode::ShowRaw) {
                draw_pointer(ren, st.cx, st.cy, GRAY, BLACK);
            } else if (idleMode == IdleCursorMode::FreezeLast) {
                draw_pointer(ren, st.last_touch_x, st.last_touch_y, GRAY, BLACK);
            } else {
                // Hide
            }
        }
        SDL_RenderPresent(ren);
        SDL_Delay(16);
    }

    // Cleanup
    fclose(pipeIn);
    int status = 0; if (pid > 0) waitpid(pid, &status, 0);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
