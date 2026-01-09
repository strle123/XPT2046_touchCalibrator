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
#include <signal.h>

static std::string get_exe_dir() {
	char buf[4096];
	ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (n <= 0) return std::string();
	buf[n] = '\0';
	std::string path(buf);
	size_t pos = path.find_last_of('/');
	if (pos == std::string::npos) return std::string();
	return path.substr(0, pos);
}

static std::string find_config_path() {
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

struct Config {
	int min_x = 0, max_x = 4095, min_y = 0, max_y = 4095;
	int screen_w = 800, screen_h = 480;
	int deadzone_left = 0, deadzone_right = 0, deadzone_top = 0, deadzone_bottom = 0;
};

static void load_config(const std::string& cfgPath, Config& cfg) {
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
		auto ltrim = [](std::string& s) { s.erase(0, s.find_first_not_of(" \t")); };
		auto rtrim = [](std::string& s) { s.erase(s.find_last_not_of(" \t") + 1); };
		ltrim(key);
		rtrim(key);
		ltrim(val);
		rtrim(val);
		int iv = std::atoi(val.c_str());

		if (key == "min_x") cfg.min_x = iv;
		else if (key == "max_x") cfg.max_x = iv;
		else if (key == "min_y") cfg.min_y = iv;
		else if (key == "max_y") cfg.max_y = iv;
		else if (key == "screen_w") cfg.screen_w = iv;
		else if (key == "screen_h") cfg.screen_h = iv;
		else if (key == "deadzone_left") cfg.deadzone_left = iv;
		else if (key == "deadzone_right") cfg.deadzone_right = iv;
		else if (key == "deadzone_top") cfg.deadzone_top = iv;
		else if (key == "deadzone_bottom") cfg.deadzone_bottom = iv;
	}

	if (cfg.max_x <= cfg.min_x) cfg.max_x = cfg.min_x + 1;
	if (cfg.max_y <= cfg.min_y) cfg.max_y = cfg.min_y + 1;
	if (cfg.screen_w < 1) cfg.screen_w = 1;
	if (cfg.screen_h < 1) cfg.screen_h = 1;
}

static std::string find_calibrator_binary() {
	std::vector<std::string> candidates = {
		get_exe_dir() + "/xpt2046_calibrator",
		get_exe_dir() + "/../build/xpt2046_calibrator",
		"xpt2046_calibrator"};
	for (const auto& c : candidates) {
		std::ifstream f(c);
		if (f.good()) return c;
	}
	return std::string();
}

static int clamp_i(int v, int lo, int hi) {
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

static std::pair<int, int> scale_raw_to_screen(int raw_x, int raw_y, const Config& cfg, int invert_x, int invert_y, int swap_xy) {
	int x = raw_x;
	int y = raw_y;
	if (swap_xy) {
		int tmp = x;
		x = y;
		y = tmp;
	}
	if (invert_x) x = 4095 - x;
	if (invert_y) y = 4095 - y;
	x = clamp_i(x, cfg.min_x, cfg.max_x);
	y = clamp_i(y, cfg.min_y, cfg.max_y);

	int sx = (int)((x - cfg.min_x) * (cfg.screen_w / (double)(cfg.max_x - cfg.min_x)));
	int sy = (int)((y - cfg.min_y) * (cfg.screen_h / (double)(cfg.max_y - cfg.min_y)));
	if (sx < 0) sx = 0;
	if (sy < 0) sy = 0;
	if (sx >= cfg.screen_w) sx = cfg.screen_w - 1;
	if (sy >= cfg.screen_h) sy = cfg.screen_h - 1;
	return {sx, sy};
}

static void draw_pointer(SDL_Renderer* r, int x, int y, SDL_Color col, SDL_Color border) {
	SDL_Rect dot{x - 6, y - 6, 12, 12};
	SDL_SetRenderDrawColor(r, col.r, col.g, col.b, 255);
	SDL_RenderFillRect(r, &dot);
	SDL_SetRenderDrawColor(r, border.r, border.g, border.b, 255);
	SDL_RenderDrawRect(r, &dot);
}

static void draw_rect_outline(SDL_Renderer* r, SDL_Rect rect, SDL_Color col) {
	SDL_SetRenderDrawColor(r, col.r, col.g, col.b, 255);
	SDL_RenderDrawRect(r, &rect);
}

// Minimal 5x7 font for digits + a few uppercase letters used in the UI.
static const unsigned char* glyph_5x7(char c) {
	// Each glyph is 7 rows, 5 bits per row (MSB on the left).
	static unsigned char blank[7] = {0, 0, 0, 0, 0, 0, 0};
	static unsigned char colon[7] = {0, 0x04, 0, 0, 0x04, 0, 0};
	static unsigned char dash[7] = {0, 0, 0, 0x1F, 0, 0, 0};

	static unsigned char zero[7] = {0x1E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x1E};
	static unsigned char one[7] = {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
	static unsigned char two[7] = {0x1E, 0x11, 0x01, 0x0E, 0x10, 0x10, 0x1F};
	static unsigned char three[7] = {0x1E, 0x11, 0x01, 0x0E, 0x01, 0x11, 0x1E};
	static unsigned char four[7] = {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
	static unsigned char five[7] = {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x1E};
	static unsigned char six[7] = {0x0E, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x0E};
	static unsigned char seven[7] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
	static unsigned char eight[7] = {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
	static unsigned char nine[7] = {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x11, 0x0E};

	static unsigned char A[7] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
	static unsigned char D[7] = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
	static unsigned char G[7] = {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E};
	static unsigned char N[7] = {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
	static unsigned char O[7] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
	static unsigned char P[7] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
	static unsigned char R[7] = {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
	static unsigned char T[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
	static unsigned char U[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
	static unsigned char W[7] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A};
	static unsigned char X[7] = {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11};
	static unsigned char Y[7] = {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};

	if (c == ' ') return blank;
	if (c == ':') return colon;
	if (c == '-') return dash;

	if (c >= '0' && c <= '9') {
		switch (c) {
			case '0': return zero;
			case '1': return one;
			case '2': return two;
			case '3': return three;
			case '4': return four;
			case '5': return five;
			case '6': return six;
			case '7': return seven;
			case '8': return eight;
			case '9': return nine;
		}
	}

	switch (c) {
		case 'A': return A;
		case 'D': return D;
		case 'G': return G;
		case 'N': return N;
		case 'O': return O;
		case 'P': return P;
		case 'R': return R;
		case 'T': return T;
		case 'U': return U;
		case 'W': return W;
		case 'X': return X;
		case 'Y': return Y;
		default: return blank;
	}
}

static void draw_text_5x7(SDL_Renderer* r, int x, int y, const std::string& s, SDL_Color col, int scale = 2) {
	SDL_SetRenderDrawColor(r, col.r, col.g, col.b, 255);
	int cx = x;
	for (char c : s) {
		const unsigned char* g = glyph_5x7(c);
		for (int row = 0; row < 7; ++row) {
			unsigned char bits = g[row];
			for (int colb = 0; colb < 5; ++colb) {
				bool on = (bits & (1u << (4 - colb))) != 0;
				if (on) {
					SDL_Rect px{cx + colb * scale, y + row * scale, scale, scale};
					SDL_RenderFillRect(r, &px);
				}
			}
		}
		cx += 6 * scale;
	}
}

int main(int argc, char** argv) {
	(void)argc;
	(void)argv;

	Config cfg;
	std::string cfgPath = find_config_path();
	load_config(cfgPath, cfg);

	// These come from Basic settings / wizard and must affect the gray pointer too.
	int invert_x = 0;
	int invert_y = 0;
	int swap_xy = 0;
	if (const char* v = getenv("XPT_INVERT_X")) { if (*v) invert_x = std::atoi(v); }
	if (const char* v = getenv("XPT_INVERT_Y")) { if (*v) invert_y = std::atoi(v); }
	if (const char* v = getenv("XPT_SWAP_XY")) { if (*v) swap_xy = std::atoi(v); }

	// Allow env overrides for live testing
	if (const char* v = getenv("XPT_MIN_X")) { if (*v) cfg.min_x = std::atoi(v); }
	if (const char* v = getenv("XPT_MAX_X")) { if (*v) cfg.max_x = std::atoi(v); }
	if (const char* v = getenv("XPT_MIN_Y")) { if (*v) cfg.min_y = std::atoi(v); }
	if (const char* v = getenv("XPT_MAX_Y")) { if (*v) cfg.max_y = std::atoi(v); }
	if (const char* v = getenv("XPT_SCREEN_W")) { if (*v) cfg.screen_w = std::atoi(v); }
	if (const char* v = getenv("XPT_SCREEN_H")) { if (*v) cfg.screen_h = std::atoi(v); }
	if (const char* v = getenv("XPT_DEADZONE_LEFT")) { if (*v) cfg.deadzone_left = std::atoi(v); }
	if (const char* v = getenv("XPT_DEADZONE_RIGHT")) { if (*v) cfg.deadzone_right = std::atoi(v); }
	if (const char* v = getenv("XPT_DEADZONE_TOP")) { if (*v) cfg.deadzone_top = std::atoi(v); }
	if (const char* v = getenv("XPT_DEADZONE_BOTTOM")) { if (*v) cfg.deadzone_bottom = std::atoi(v); }

	if (cfg.max_x <= cfg.min_x) cfg.max_x = cfg.min_x + 1;
	if (cfg.max_y <= cfg.min_y) cfg.max_y = cfg.min_y + 1;
	if (cfg.screen_w < 1) cfg.screen_w = 1;
	if (cfg.screen_h < 1) cfg.screen_h = 1;

	const char* disp = getenv("DISPLAY");
	if (disp && *disp) setenv("SDL_VIDEO_X11_XSHM", "0", 0);

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
		std::fprintf(stderr, "[ERROR] SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}

	SDL_Window* win = SDL_CreateWindow("XPT2046 Advanced GUI (SDL2)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, cfg.screen_w, cfg.screen_h, 0);
	if (!win) {
		std::fprintf(stderr, "[ERROR] Failed to create window: %s\n", SDL_GetError());
		SDL_Quit();
		return 1;
	}

	SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
	if (!ren) {
		std::fprintf(stderr, "[ERROR] Failed to create renderer: %s\n", SDL_GetError());
		SDL_DestroyWindow(win);
		SDL_Quit();
		return 1;
	}

	SDL_Color BLACK{0, 0, 0, 255};
	SDL_Color GRAY{160, 160, 160, 255};
	SDL_Color RED{220, 60, 60, 255};
	SDL_Color BLUE{66, 135, 245, 255};
	SDL_Color GREEN{48, 173, 86, 255};

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
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		setenv("CALIBRATION_RUNNING", "1", 1);
		if (!cfgPath.empty()) setenv("TOUCH_CONFIG_PATH", cfgPath.c_str(), 1);

		const char* ix = getenv("XPT_INVERT_X");
		const char* iy = getenv("XPT_INVERT_Y");
		const char* sw = getenv("XPT_SWAP_XY");

		std::vector<const char*> args;
		args.push_back(calibrator.c_str());
		args.push_back("--advanced_raw");
		if (ix && *ix) { args.push_back("--invert_x"); args.push_back(ix); }
		if (iy && *iy) { args.push_back("--invert_y"); args.push_back(iy); }
		if (sw && *sw) { args.push_back("--swap_xy"); args.push_back(sw); }
		args.push_back(nullptr);
		execv(calibrator.c_str(), const_cast<char* const*>(args.data()));
		std::perror("execv");
		_exit(127);
	}

	close(pipefd[1]);
	FILE* pipeIn = fdopen(pipefd[0], "r");
	int pipe_fd = fileno(pipeIn);
	int flags = fcntl(pipe_fd, F_GETFL, 0);
	fcntl(pipe_fd, F_SETFL, flags | O_NONBLOCK);

	char buf[2048];

	std::regex re_spi(R"(^\[SPI\]\s+XPT2046\s+X:\s*(\d+)\s+Y:\s*(\d+)\s+\(raw X:\s*(\d+)\s+raw Y:\s*(\d+)(?:\s+SX:\s*(\d+)\s+SY:\s*(\d+))?(?:\s+Z:\s*(\d+))?(?:\s+DOWN:\s*(\d+))?.*\))");
	std::regex re_gesture(R"(^\[GESTURE\]\s+(.+)$)");

	int out_x = cfg.screen_w / 2, out_y = cfg.screen_h / 2;
	int raw_x = 0, raw_y = 0;
	int pressure = 0;
	bool down = false;
	std::string last_gesture;

	bool running = true;
	while (running) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			if (e.type == SDL_QUIT) running = false;
			if (e.type == SDL_KEYDOWN) {
				SDL_Keycode k = e.key.keysym.sym;
				if (k == SDLK_ESCAPE || k == SDLK_q) running = false;
			}
		}

		ssize_t n = read(pipe_fd, buf, sizeof(buf) - 1);
		if (n > 0) {
			buf[n] = '\0';
			std::istringstream iss{std::string(buf)};
			std::string line;
			while (std::getline(iss, line)) {
				std::smatch m;
				if (std::regex_search(line, m, re_spi)) {
					int x_raw_axis = std::atoi(m[1].str().c_str());
					int y_raw_axis = std::atoi(m[2].str().c_str());
					raw_x = std::atoi(m[3].str().c_str());
					raw_y = std::atoi(m[4].str().c_str());
					int sx = (m.size() > 5 && m[5].matched) ? std::atoi(m[5].str().c_str()) : -1;
					int sy = (m.size() > 6 && m[6].matched) ? std::atoi(m[6].str().c_str()) : -1;
					pressure = (m.size() > 7 && m[7].matched) ? std::atoi(m[7].str().c_str()) : 0;
					int down_i = (m.size() > 8 && m[8].matched) ? std::atoi(m[8].str().c_str()) : -1;
					if (down_i == 0 || down_i == 1) down = (down_i == 1);

					if (sx >= 0 && sy >= 0) {
						out_x = sx;
						out_y = sy;
					} else {
						// Fallback: map raw-axis X/Y to screen (no advanced pipeline).
						auto p = scale_raw_to_screen(x_raw_axis, y_raw_axis, cfg, invert_x, invert_y, swap_xy);
						out_x = p.first;
						out_y = p.second;
					}

					if (out_x < 0) out_x = 0;
					if (out_y < 0) out_y = 0;
					if (out_x >= cfg.screen_w) out_x = cfg.screen_w - 1;
					if (out_y >= cfg.screen_h) out_y = cfg.screen_h - 1;
				} else if (std::regex_search(line, m, re_gesture)) {
					last_gesture = m[1].str();
				}
			}
		}

		SDL_SetRenderDrawColor(ren, 245, 245, 245, 255);
		SDL_RenderClear(ren);

		// Deadzone rectangle
		SDL_Rect dz{
			cfg.deadzone_left,
			cfg.deadzone_top,
			cfg.screen_w - cfg.deadzone_left - cfg.deadzone_right,
			cfg.screen_h - cfg.deadzone_top - cfg.deadzone_bottom};
		if (dz.w < 1) dz.w = 1;
		if (dz.h < 1) dz.h = 1;
		draw_rect_outline(ren, dz, BLUE);

		// Pressure bar
		int bar_w = (int)((pressure / 4095.0) * (cfg.screen_w - 20));
		if (bar_w < 0) bar_w = 0;
		if (bar_w > cfg.screen_w - 20) bar_w = cfg.screen_w - 20;
		SDL_Rect bar_bg{10, 10, cfg.screen_w - 20, 10};
		SDL_Rect bar_fg{10, 10, bar_w, 10};
		SDL_SetRenderDrawColor(ren, 220, 220, 220, 255);
		SDL_RenderFillRect(ren, &bar_bg);
		SDL_SetRenderDrawColor(ren, GREEN.r, GREEN.g, GREEN.b, 255);
		SDL_RenderFillRect(ren, &bar_fg);
		draw_rect_outline(ren, bar_bg, BLACK);

		// Raw-mapped pointer (gray) and output pointer (red)
		// Gray uses the same swap/invert + min/max mapping as the calibrator (pre-advanced pipeline).
		auto raw_pos = scale_raw_to_screen(raw_x, raw_y, cfg, invert_x, invert_y, swap_xy);
		draw_pointer(ren, raw_pos.first, raw_pos.second, GRAY, BLACK);
		if (down) draw_pointer(ren, out_x, out_y, RED, BLACK);

		// Minimal on-screen text
		draw_text_5x7(ren, 10, 28, std::string("RAW:"), BLACK, 2);
		draw_text_5x7(ren, 10 + 6 * 2 * 4, 28, std::to_string(raw_pos.first) + ":" + std::to_string(raw_pos.second), BLACK, 2);
		draw_text_5x7(ren, 10, 44, std::string("OUT:"), BLACK, 2);
		draw_text_5x7(ren, 10 + 6 * 2 * 4, 44, std::to_string(out_x) + ":" + std::to_string(out_y), BLACK, 2);
		draw_text_5x7(ren, 10, 60, std::string("DOWN:"), BLACK, 2);
		draw_text_5x7(ren, 10 + 6 * 2 * 5, 60, down ? "1" : "0", BLACK, 2);

		if (!last_gesture.empty()) {
			draw_text_5x7(ren, 10, 76, std::string("G:"), BLACK, 2);
			// Only draw a shortened gesture string with supported characters
			std::string g;
			for (char c : last_gesture) {
				if ((c >= '0' && c <= '9') || c == ' ' || c == ':' || c == '-' || (c >= 'A' && c <= 'Z')) g.push_back(c);
			}
			if ((int)g.size() > 30) g.resize(30);
			draw_text_5x7(ren, 10 + 6 * 2 * 2, 76, g, BLACK, 2);
		}

		SDL_RenderPresent(ren);
		SDL_Delay(16);
	}

	if (pid > 0) {
		kill(pid, SIGTERM);
		int st = 0;
		waitpid(pid, &st, 0);
	}

	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return 0;
}