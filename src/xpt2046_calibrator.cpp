#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <cstdint>
#include <cerrno>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <sys/stat.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>


// Try to find touch_config.txt in common locations
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

static bool path_exists(const std::string& p) {
	struct stat st;
	return ::stat(p.c_str(), &st) == 0;
}

static std::string find_config_path() {
	const char* envPath = getenv("TOUCH_CONFIG_PATH");
	if (envPath && *envPath) {
		std::ifstream f(envPath);
		if (f.good()) return std::string(envPath);
	}

	std::vector<std::string> candidates;
	// System-wide config (useful when running as a system service)
	candidates.push_back("/etc/xpt2046/touch_config.txt");
	// Relative to current working directory
	candidates.push_back("touch_config.txt");
	candidates.push_back("installation/touch_config.txt");

	// Relative to executable directory (binary usually in build/, config in ../installation/)
	std::string exeDir = get_exe_dir();
	if (!exeDir.empty()) {
		candidates.push_back(exeDir + "/touch_config.txt");
		candidates.push_back(exeDir + "/installation/touch_config.txt");
		candidates.push_back(exeDir + "/../installation/touch_config.txt");
	}

	for (const auto& p : candidates) {
		std::ifstream f(p);
		if (f.good()) return p;
	}
	return std::string();
}

// Update or append spi_device in config file
static void update_config_spi(const std::string& cfgPath, const std::string& devPath) {
	if (cfgPath.empty() || devPath.empty()) return;
	std::ifstream in(cfgPath);
	std::vector<std::string> lines;
	bool replaced = false;
	if (in.good()) {
		std::string line;
		while (std::getline(in, line)) {
			if (line.rfind("spi_device=", 0) == 0) {
				lines.push_back(std::string("spi_device=") + devPath);
				replaced = true;
			} else {
				lines.push_back(line);
			}
		}
		in.close();
	}
	if (!replaced) {
		lines.push_back(std::string("spi_device=") + devPath);
	}
	std::ofstream out(cfgPath, std::ios::trunc);
	for (size_t i = 0; i < lines.size(); ++i) {
		out << lines[i] << "\n";
	}
}

static std::string default_config_save_path(const std::string& existingCfg) {
	if (!existingCfg.empty()) return existingCfg;
	std::string exeDir = get_exe_dir();
	if (!exeDir.empty()) {
		std::string p = exeDir + "/../installation/touch_config.txt";
		return p;
	}
	return std::string("installation/touch_config.txt");
}

static bool parse_int(const std::string& s, int& out) {
	char* end = nullptr;
	long v = std::strtol(s.c_str(), &end, 10);
	if (end == s.c_str()) return false;
	out = (int)v;
	return true;
}

static bool parse_float(const std::string& s, float& out) {
	char* end = nullptr;
	float v = std::strtof(s.c_str(), &end);
	if (end == s.c_str()) return false;
	out = v;
	return true;
}

template <typename T>
static T clamp_val(T v, T lo, T hi) {
	return (v < lo) ? lo : ((v > hi) ? hi : v);
}

struct AdvancedParams {
	int screen_w = 800;
	int screen_h = 480;
	int poll_us = 100000; // output/update interval; lower = faster, higher = less CPU/log spam

	int offset_x = 0;
	int offset_y = 0;
	float scale_x = 1.0f;
	float scale_y = 1.0f;

	int deadzone_left = 0;
	int deadzone_right = 0;
	int deadzone_top = 0;
	int deadzone_bottom = 0;

	int median_window = 3; // 0,3,5
	float iir_alpha = 0.20f; // 0..1 (0 disables)

	int press_threshold = 120;
	int release_threshold = 80;

	int max_delta_px = 0; // 0 disables

	int tap_max_ms = 250;
	int tap_max_move_px = 12;
	int drag_start_px = 18;
};

static void apply_env_overrides(int& invert_x, int& invert_y, int& swap_xy,
						int& min_x, int& max_x, int& min_y, int& max_y,
						AdvancedParams& adv,
						std::string& spi_device_cfg) {
	auto env_i = [](const char* name, int& dst) {
		const char* v = getenv(name);
		if (v && *v) dst = std::atoi(v);
	};
	auto env_f = [](const char* name, float& dst) {
		const char* v = getenv(name);
		if (v && *v) dst = std::strtof(v, nullptr);
	};

	if (const char* v = getenv("XPT_SPI_DEVICE")) { if (*v) spi_device_cfg = v; }

	env_i("XPT_INVERT_X", invert_x);
	env_i("XPT_INVERT_Y", invert_y);
	env_i("XPT_SWAP_XY", swap_xy);
	env_i("XPT_MIN_X", min_x);
	env_i("XPT_MAX_X", max_x);
	env_i("XPT_MIN_Y", min_y);
	env_i("XPT_MAX_Y", max_y);

	env_i("XPT_SCREEN_W", adv.screen_w);
	env_i("XPT_SCREEN_H", adv.screen_h);
	env_i("XPT_POLL_US", adv.poll_us);
	env_i("XPT_OFFSET_X", adv.offset_x);
	env_i("XPT_OFFSET_Y", adv.offset_y);
	env_f("XPT_SCALE_X", adv.scale_x);
	env_f("XPT_SCALE_Y", adv.scale_y);
	env_i("XPT_DEADZONE_LEFT", adv.deadzone_left);
	env_i("XPT_DEADZONE_RIGHT", adv.deadzone_right);
	env_i("XPT_DEADZONE_TOP", adv.deadzone_top);
	env_i("XPT_DEADZONE_BOTTOM", adv.deadzone_bottom);
	env_i("XPT_MEDIAN_WINDOW", adv.median_window);
	env_f("XPT_IIR_ALPHA", adv.iir_alpha);
	env_i("XPT_PRESS_THRESHOLD", adv.press_threshold);
	env_i("XPT_RELEASE_THRESHOLD", adv.release_threshold);
	env_i("XPT_MAX_DELTA_PX", adv.max_delta_px);
	env_i("XPT_TAP_MAX_MS", adv.tap_max_ms);
	env_i("XPT_TAP_MAX_MOVE_PX", adv.tap_max_move_px);
	env_i("XPT_DRAG_START_PX", adv.drag_start_px);
}

// Load key=value config for invert/swap and optional ranges + advanced params
static void load_config(int& invert_x, int& invert_y, int& swap_xy,
						int& min_x, int& max_x, int& min_y, int& max_y,
						AdvancedParams& adv,
						std::string& usedPath,
						std::string& spi_device_cfg) {
	usedPath = find_config_path();
	if (usedPath.empty()) {
		std::cerr << "[INFO] No touch_config.txt found; using defaults." << std::endl;
		return;
	}
	std::ifstream in(usedPath);
	if (!in.good()) {
		std::cerr << "[INFO] touch_config.txt not readable; using defaults." << std::endl;
		usedPath.clear();
		return;
	}
	std::string line;
	while (std::getline(in, line)) {
		if (line.empty()) continue;
		// Skip comments and malformed lines
		if (line[0] == '#') continue;
		auto pos = line.find('=');
		if (pos == std::string::npos) continue;
		std::string key = line.substr(0, pos);
		std::string val = line.substr(pos + 1);
		// trim spaces
		auto ltrim = [](std::string& s){ s.erase(0, s.find_first_not_of(" \t")); };
		auto rtrim = [](std::string& s){ s.erase(s.find_last_not_of(" \t") + 1); };
		ltrim(key); rtrim(key); ltrim(val); rtrim(val);
		int iv = 0;
		float fv = 0.0f;
		if (key == "invert_x" && parse_int(val, iv)) invert_x = iv;
		else if (key == "invert_y" && parse_int(val, iv)) invert_y = iv;
		else if (key == "swap_xy" && parse_int(val, iv)) swap_xy = iv;
		else if (key == "min_x" && parse_int(val, iv)) min_x = iv;
		else if (key == "max_x" && parse_int(val, iv)) max_x = iv;
		else if (key == "min_y" && parse_int(val, iv)) min_y = iv;
		else if (key == "max_y" && parse_int(val, iv)) max_y = iv;
		else if (key == "spi_device") spi_device_cfg = val;
		else if (key == "screen_w" && parse_int(val, iv)) adv.screen_w = iv;
		else if (key == "screen_h" && parse_int(val, iv)) adv.screen_h = iv;
		else if (key == "poll_us" && parse_int(val, iv)) adv.poll_us = iv;
		else if (key == "offset_x" && parse_int(val, iv)) adv.offset_x = iv;
		else if (key == "offset_y" && parse_int(val, iv)) adv.offset_y = iv;
		else if (key == "scale_x" && parse_float(val, fv)) adv.scale_x = fv;
		else if (key == "scale_y" && parse_float(val, fv)) adv.scale_y = fv;
		else if (key == "deadzone_left" && parse_int(val, iv)) adv.deadzone_left = iv;
		else if (key == "deadzone_right" && parse_int(val, iv)) adv.deadzone_right = iv;
		else if (key == "deadzone_top" && parse_int(val, iv)) adv.deadzone_top = iv;
		else if (key == "deadzone_bottom" && parse_int(val, iv)) adv.deadzone_bottom = iv;
		else if (key == "median_window" && parse_int(val, iv)) adv.median_window = iv;
		else if (key == "iir_alpha" && parse_float(val, fv)) adv.iir_alpha = fv;
		else if (key == "press_threshold" && parse_int(val, iv)) adv.press_threshold = iv;
		else if (key == "release_threshold" && parse_int(val, iv)) adv.release_threshold = iv;
		else if (key == "max_delta_px" && parse_int(val, iv)) adv.max_delta_px = iv;
		else if (key == "tap_max_ms" && parse_int(val, iv)) adv.tap_max_ms = iv;
		else if (key == "tap_max_move_px" && parse_int(val, iv)) adv.tap_max_move_px = iv;
		else if (key == "drag_start_px" && parse_int(val, iv)) adv.drag_start_px = iv;
	}
}

// Minimal SPI read for XPT2046 (read X or Y position)
int read_xpt2046(int spi_fd, uint8_t command) {
	uint8_t tx[3] = { command, 0x00, 0x00 };
	uint8_t rx[3] = { 0 };
	struct spi_ioc_transfer tr = {};
	tr.tx_buf = (unsigned long)tx;
	tr.rx_buf = (unsigned long)rx;
	tr.len = 3;
	tr.speed_hz = 1000000;
	tr.bits_per_word = 8;
	tr.delay_usecs = 0;

	int ret = ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tr);
	if (ret < 1) {
		std::cerr << "[ERROR] SPI transfer failed" << std::endl;
		return -1;
	}
	int value = ((rx[1] << 5) | (rx[2] >> 3)) & 0xFFF;
	return value;
}

int main(int argc, char* argv[]) {
	// Defaults; will be overridden by config then CLI args
	int invert_x = 0, invert_y = 0, swap_xy = 0;
	int min_x = 0, max_x = 4095, min_y = 0, max_y = 4095;
	AdvancedParams adv;
	std::string cfgPath; std::string spi_device_cfg;
	load_config(invert_x, invert_y, swap_xy, min_x, max_x, min_y, max_y, adv, cfgPath, spi_device_cfg);
	// Parse CLI args (override config if provided)
	int probe_seconds = 0;
	bool advanced_raw = false;
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--invert_x") == 0 && i+1 < argc) invert_x = atoi(argv[++i]);
		if (strcmp(argv[i], "--invert_y") == 0 && i+1 < argc) invert_y = atoi(argv[++i]);
		if (strcmp(argv[i], "--swap_xy") == 0 && i+1 < argc) swap_xy = atoi(argv[++i]);
		if (strcmp(argv[i], "--spi_device") == 0 && i+1 < argc) spi_device_cfg = argv[++i];
		if (strcmp(argv[i], "--probe") == 0 && i+1 < argc) probe_seconds = atoi(argv[++i]);
		if (strcmp(argv[i], "--advanced_raw") == 0) advanced_raw = true;
		if (strcmp(argv[i], "--screen_w") == 0 && i+1 < argc) adv.screen_w = atoi(argv[++i]);
		if (strcmp(argv[i], "--screen_h") == 0 && i+1 < argc) adv.screen_h = atoi(argv[++i]);
		if (strcmp(argv[i], "--poll_us") == 0 && i+1 < argc) adv.poll_us = atoi(argv[++i]);
		if (strcmp(argv[i], "--offset_x") == 0 && i+1 < argc) adv.offset_x = atoi(argv[++i]);
		if (strcmp(argv[i], "--offset_y") == 0 && i+1 < argc) adv.offset_y = atoi(argv[++i]);
		if (strcmp(argv[i], "--scale_x") == 0 && i+1 < argc) adv.scale_x = std::strtof(argv[++i], nullptr);
		if (strcmp(argv[i], "--scale_y") == 0 && i+1 < argc) adv.scale_y = std::strtof(argv[++i], nullptr);
		if (strcmp(argv[i], "--deadzone_left") == 0 && i+1 < argc) adv.deadzone_left = atoi(argv[++i]);
		if (strcmp(argv[i], "--deadzone_right") == 0 && i+1 < argc) adv.deadzone_right = atoi(argv[++i]);
		if (strcmp(argv[i], "--deadzone_top") == 0 && i+1 < argc) adv.deadzone_top = atoi(argv[++i]);
		if (strcmp(argv[i], "--deadzone_bottom") == 0 && i+1 < argc) adv.deadzone_bottom = atoi(argv[++i]);
		if (strcmp(argv[i], "--median_window") == 0 && i+1 < argc) adv.median_window = atoi(argv[++i]);
		if (strcmp(argv[i], "--iir_alpha") == 0 && i+1 < argc) adv.iir_alpha = std::strtof(argv[++i], nullptr);
		if (strcmp(argv[i], "--press_threshold") == 0 && i+1 < argc) adv.press_threshold = atoi(argv[++i]);
		if (strcmp(argv[i], "--release_threshold") == 0 && i+1 < argc) adv.release_threshold = atoi(argv[++i]);
		if (strcmp(argv[i], "--max_delta_px") == 0 && i+1 < argc) adv.max_delta_px = atoi(argv[++i]);
		if (strcmp(argv[i], "--tap_max_ms") == 0 && i+1 < argc) adv.tap_max_ms = atoi(argv[++i]);
		if (strcmp(argv[i], "--tap_max_move_px") == 0 && i+1 < argc) adv.tap_max_move_px = atoi(argv[++i]);
		if (strcmp(argv[i], "--drag_start_px") == 0 && i+1 < argc) adv.drag_start_px = atoi(argv[++i]);
	}

	// Environment overrides for live testing without saving
	apply_env_overrides(invert_x, invert_y, swap_xy, min_x, max_x, min_y, max_y, adv, spi_device_cfg);

	adv.screen_w = clamp_val(adv.screen_w, 1, 4096);
	adv.screen_h = clamp_val(adv.screen_h, 1, 4096);
	adv.poll_us = clamp_val(adv.poll_us, 1000, 1000000);
	adv.scale_x = clamp_val(adv.scale_x, 0.01f, 10.0f);
	adv.scale_y = clamp_val(adv.scale_y, 0.01f, 10.0f);
	adv.iir_alpha = clamp_val(adv.iir_alpha, 0.0f, 1.0f);
	if (!(adv.median_window == 0 || adv.median_window == 3 || adv.median_window == 5)) adv.median_window = 3;
	if (adv.release_threshold > adv.press_threshold) adv.release_threshold = adv.press_threshold;

	// If requested, run probing mode to choose best SPI and persist, then exit
	if (probe_seconds > 0) {
		const char* candidates[] = {"/dev/spidev0.1", "/dev/spidev0.0", "/dev/spidev1.0", "/dev/spidev1.1"};
		uint8_t mode = SPI_MODE_0; uint32_t speed = 1000000;
		auto try_open = [&](const char* devPath) {
			int fd = open(devPath, O_RDWR);
			if (fd < 0) return -1;
			if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0) { close(fd); return -1; }
			if (ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) { close(fd); return -1; }
			return fd;
		};
		auto print_progress_bar = [](const char* dev, int hits, int samples) {
			int percent = (samples > 0) ? (hits * 100 / samples) : 0;
			int bar_len = 20;
			int filled = percent * bar_len / 100;
			std::string bar(filled, '#');
			bar += std::string(bar_len - filled, '-');
			const char* status = "Poor";
			if (percent > 90) status = "Excellent";
			else if (percent > 70) status = "Good";
			else if (percent > 40) status = "Fair";
			printf("%s: [%s] %3d%%  %s\n", dev, bar.c_str(), percent, status);
		};
		const char* best_dev = nullptr; int best_score = -1;
		int samples = probe_seconds * 100; // 10ms per sample
		std::vector<int> all_hits(sizeof(candidates)/sizeof(candidates[0]), 0);
		// Print instruction and initial empty bars
		printf("Press and hold finger at the center of display\n");
		int num_devs = (int)(sizeof(candidates)/sizeof(candidates[0]));
		std::vector<int> hits_vec(num_devs, 0);
		for (int i = 0; i < num_devs; ++i) {
			print_progress_bar(candidates[i], 0, 1);
		}
		fflush(stdout);
		static int fds[4] = {-1,-1,-1,-1};
		// Probe loop: update bars in place.
		// A "hit" requires pressure (Z1) above threshold and non-extreme X/Y.
		for (int j = 0; j < samples; ++j) {
			for (int i = 0; i < num_devs; ++i) {
				if (j == 0) {
					fds[i] = try_open(candidates[i]);
				}
				if (fds[i] < 0) continue;
				int x = read_xpt2046(fds[i], 0x90);
				int y = read_xpt2046(fds[i], 0xD0);
				int z1 = read_xpt2046(fds[i], 0xB0);
				if (x < 0 || y < 0 || z1 < 0) continue;
				bool pressure_ok = (adv.press_threshold > 0) ? (z1 >= adv.press_threshold) : (z1 > 0);
				bool xy_ok = (x >= 50 && x <= 4045 && y >= 50 && y <= 4045);
				if (pressure_ok && xy_ok) hits_vec[i]++;
			}
			if (j % 10 == 0 || j == samples-1) {
				// Move cursor up for all bars
				for (int k = 0; k < num_devs; ++k) printf("\033[F");
				for (int i = 0; i < num_devs; ++i) {
					print_progress_bar(candidates[i], hits_vec[i], j+1);
				}
				fflush(stdout);
			}
			usleep(10000);
		}
		// Close all fds
		for (int i = 0; i < num_devs; ++i) {
			if (fds[i] >= 0) close(fds[i]);
		}
		// Calculate best device (most hits)
		int best_hits = -1;
		for (int i = 0; i < num_devs; ++i) {
			if (hits_vec[i] > best_hits) {
				best_hits = hits_vec[i];
				best_dev = candidates[i];
			}
		}
		if (best_hits <= 0) {
			std::cerr << "[ERROR] Probe failed: no valid SPI device detected (no hits)." << std::endl;
			return 1;
		}
		std::cout << "[PROBE] Selected SPI: " << best_dev << " (hits=" << best_hits << ")" << std::endl;
		std::string savePath = default_config_save_path(cfgPath);
		update_config_spi(savePath, best_dev);
		std::cout << "[CONFIG] Saved spi_device=" << best_dev << " to " << savePath << std::endl;
		return 0;
	}

	// Normal runtime starts here (after any optional probe stage)
	std::cout << "XPT2046 userspace driver start!" << std::endl;
	fflush(stdout);

	if (cfgPath.empty()) {
		std::cerr << "[INFO] No touch_config.txt found. Set TOUCH_CONFIG_PATH or run the calibration script to generate one." << std::endl;
	}

	if (!cfgPath.empty()) {
		std::cout << "[CONFIG] Using: " << cfgPath << std::endl;
	}
	std::cout << "[CONFIG] invert_x=" << invert_x
			  << " invert_y=" << invert_y
			  << " swap_xy=" << swap_xy
			  << " ranges x:[" << min_x << "," << max_x << "] y:[" << min_y << "," << max_y << "]"
			  << " screen:[" << adv.screen_w << "x" << adv.screen_h << "]"
			  << " offset:[" << adv.offset_x << "," << adv.offset_y << "]"
			  << " scale:[" << adv.scale_x << "," << adv.scale_y << "]"
			  << " deadzone:[L" << adv.deadzone_left << " R" << adv.deadzone_right << " T" << adv.deadzone_top << " B" << adv.deadzone_bottom << "]"
			  << " median=" << adv.median_window
			  << " iir_alpha=" << adv.iir_alpha
			  << " press=" << adv.press_threshold << " release=" << adv.release_threshold
			  << " tap_ms=" << adv.tap_max_ms << " tap_move=" << adv.tap_max_move_px << " drag_px=" << adv.drag_start_px
			  << std::endl;
	fflush(stdout);

	const char* spi_devices[] = {"/dev/spidev0.1", "/dev/spidev0.0", "/dev/spidev1.0", "/dev/spidev1.1"};
	int best_fd = -1;
	const char* best_spi = nullptr;
	int best_score = -1;
	uint8_t mode = SPI_MODE_0;
	uint32_t speed = 1000000;
	auto score_device = [&](int fd, const char* devPath) {
		int minx = 4095, maxx = 0, miny = 4095, maxy = 0;
		int hits = 0;
		for (int i = 0; i < 20; ++i) {
			int x = read_xpt2046(fd, 0x90);
			int y = read_xpt2046(fd, 0xD0);
			int z1 = read_xpt2046(fd, 0xB0);
			if (x < 0 || y < 0 || z1 < 0) {
				usleep(10000);
				continue;
			}
			bool pressure_ok = (adv.press_threshold > 0) ? (z1 >= adv.press_threshold) : (z1 > 0);
			bool xy_ok = (x >= 50 && x <= 4045 && y >= 50 && y <= 4045);
			if (pressure_ok && xy_ok) {
				hits++;
				if (x < minx) minx = x; if (x > maxx) maxx = x;
				if (y < miny) miny = y; if (y > maxy) maxy = y;
			}
			usleep(10000);
		}
		int range = (hits > 0) ? ((maxx - minx) + (maxy - miny)) : 0;
		std::cerr << "[DEBUG] score(" << devPath << ") hits=" << hits
				  << " range=" << range;
		if (hits > 0) {
			std::cerr << " x:[" << minx << "," << maxx << "] y:[" << miny << "," << maxy << "]";
		}
		std::cerr << std::endl;
		// If user isn't touching during scoring, hits==0: don't treat as invalid.
		return range;
	};

	auto try_open = [&](const char* devPath) {
		int fd = open(devPath, O_RDWR);
		if (fd < 0) {
			std::cerr << "[DEBUG] open(" << devPath << ") failed: " << strerror(errno) << " (errno=" << errno << ")" << std::endl;
			return -1;
		}
		if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0) {
			std::cerr << "[DEBUG] ioctl(SPI_IOC_WR_MODE) failed for " << devPath << ": " << strerror(errno) << " (errno=" << errno << ")" << std::endl;
			close(fd);
			return -1;
		}
		if (ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
			std::cerr << "[DEBUG] ioctl(SPI_IOC_WR_MAX_SPEED_HZ) failed for " << devPath << ": " << strerror(errno) << " (errno=" << errno << ")" << std::endl;
			close(fd);
			return -1;
		}
		return fd;
	};

	// Explicit device via config/env
	if (!spi_device_cfg.empty()) {
		int fd = try_open(spi_device_cfg.c_str());
		if (fd >= 0) {
			// If the user configured a device (or probe saved it), trust it.
			// Scoring depends on live touch/pressure and can be 0 when not touching, which is confusing.
			best_fd = fd;
			best_spi = spi_device_cfg.c_str();
			best_score = 0;
		} else {
			std::cerr << "[WARN] Cannot open configured SPI device: " << spi_device_cfg << ". Falling back to auto-detect." << std::endl;
		}
	}

	// Auto-detect if not set
	if (best_fd < 0) {
		for (int dev = 0; dev < (int)(sizeof(spi_devices)/sizeof(spi_devices[0])); ++dev) {
			const char* path = spi_devices[dev];
			int fd = try_open(path);
			if (fd < 0) continue;
			int sc = score_device(fd, path);
			if (sc > best_score) {
				if (best_fd >= 0) close(best_fd);
				best_fd = fd; best_spi = path; best_score = sc;
			} else {
				close(fd);
			}
		}
	}
	if (best_fd < 0) {
		// Fallback: try typical CE1 then CE0 without sample validation
		const char* fallbacks[] = {"/dev/spidev0.1", "/dev/spidev0.0"};
		for (int i = 0; i < 2 && best_fd < 0; ++i) {
			int fd = try_open(fallbacks[i]);
			if (fd >= 0) {
				best_fd = fd; best_spi = fallbacks[i]; best_score = 0;
				std::cerr << "[WARN] Autodetection found no valid samples. Using fallback: " << best_spi << std::endl;
			}
		}
	}
	if (best_fd < 0) {
		std::cerr << "[ERROR] Failed to open or read from SPI devices. Ensure SPI is enabled (raspi-config), CS wiring is correct, and /dev/spidev* permissions are allowed." << std::endl;
		std::cerr << "[HINT] If your XPT2046 is on CE1, try setting spi_device=/dev/spidev0.1 in the config or XPT_SPI_DEVICE in the environment." << std::endl;
		return 1;
	}
	// Persist detected device to config if not set explicitly
	if (!cfgPath.empty() && spi_device_cfg.empty()) {
		update_config_spi(cfgPath, best_spi);
		std::cout << "[CONFIG] Saved spi_device=" << best_spi << " to " << cfgPath << std::endl;
	}
	std::cout << "[OK] SPI device selected: " << best_spi << std::endl;
	fflush(stdout);
	std::cout << "Press Ctrl+C to stop test..." << std::endl;
	fflush(stdout);
	bool warned_dead = false;
	bool warned_static = false;
	int extreme_count = 0;
	int same_count = 0;
	int last_x = -1, last_y = -1;
	bool touch_down = false;
	bool dragging = false;
	int down_start_x = 0, down_start_y = 0;
	auto down_start_t = std::chrono::steady_clock::now();

	std::deque<int> hist_x;
	std::deque<int> hist_y;
	bool have_filtered = false;
	int filt_x = 0, filt_y = 0;

	auto now_ms = []() -> int64_t {
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	};
	auto median_of = [](std::deque<int> v) -> int {
		if (v.empty()) return 0;
		std::sort(v.begin(), v.end());
		return v[v.size()/2];
	};
	auto dist2 = [](int ax, int ay, int bx, int by) -> int {
		int dx = ax - bx;
		int dy = ay - by;
		return dx*dx + dy*dy;
	};
	const int drag_start2 = adv.drag_start_px * adv.drag_start_px;
	const int tap_move2 = adv.tap_max_move_px * adv.tap_max_move_px;

	while (true) {
		int raw_x = read_xpt2046(best_fd, 0x90); // X command
		int raw_y = read_xpt2046(best_fd, 0xD0); // Y command
		int z1 = read_xpt2046(best_fd, 0xB0);
		int z2 = read_xpt2046(best_fd, 0xC0);
		int pressure = (z1 >= 0) ? z1 : 0;
		int x = raw_x, y = raw_y;
		if (swap_xy) {
			int tmp = x; x = y; y = tmp;
		}
		if (invert_x) x = 4095 - x;
		if (invert_y) y = 4095 - y;
		// Optional clamp using ranges if provided (apply after invert/swap on screen axes)
		if (min_x <= max_x) {
			if (x < min_x) x = min_x;
			if (x > max_x) x = max_x;
		}
		if (min_y <= max_y) {
			if (y < min_y) y = min_y;
			if (y > max_y) y = max_y;
		}
		// Map to screen space for downstream tuning
		int sx = (int)std::llround((x - min_x) * (adv.screen_w / (double)(max_x - min_x)));
		int sy = (int)std::llround((y - min_y) * (adv.screen_h / (double)(max_y - min_y)));
		sx = clamp_val(sx, 0, adv.screen_w - 1);
		sy = clamp_val(sy, 0, adv.screen_h - 1);

		// Apply offset/scale in screen space
		sx = (int)std::llround(sx * adv.scale_x + adv.offset_x);
		sy = (int)std::llround(sy * adv.scale_y + adv.offset_y);

		// Apply deadzones by clamping away from edges
		int min_sx = clamp_val(adv.deadzone_left, 0, adv.screen_w - 1);
		int max_sx = clamp_val(adv.screen_w - 1 - adv.deadzone_right, 0, adv.screen_w - 1);
		int min_sy = clamp_val(adv.deadzone_top, 0, adv.screen_h - 1);
		int max_sy = clamp_val(adv.screen_h - 1 - adv.deadzone_bottom, 0, adv.screen_h - 1);
		if (max_sx < min_sx) max_sx = min_sx;
		if (max_sy < min_sy) max_sy = min_sy;
		sx = clamp_val(sx, min_sx, max_sx);
		sy = clamp_val(sy, min_sy, max_sy);

		// Touch state (pressure hysteresis). If thresholds disabled, fallback to coordinate heuristic.
		if (adv.press_threshold > 0) {
			if (!touch_down) {
				touch_down = (pressure >= adv.press_threshold);
				if (touch_down) {
					down_start_t = std::chrono::steady_clock::now();
					down_start_x = sx;
					down_start_y = sy;
					dragging = false;
				}
			} else {
				if (pressure <= adv.release_threshold) {
					// Release
					int64_t dur = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
						std::chrono::steady_clock::now() - down_start_t).count();
					int moved2 = dist2(down_start_x, down_start_y, sx, sy);
					if (!dragging && dur <= adv.tap_max_ms && moved2 <= tap_move2) {
						std::cout << "[GESTURE] TAP X: " << sx << " Y: " << sy << " ms: " << dur << "\n";
					}
					touch_down = false;
					dragging = false;
					// Reset filters on release
					hist_x.clear();
					hist_y.clear();
					have_filtered = false;
				}
			}
		} else {
			bool present = (raw_x >= 50 && raw_x <= 4045 && raw_y >= 50 && raw_y <= 4045);
			if (!touch_down && present) {
				touch_down = true;
				down_start_t = std::chrono::steady_clock::now();
				down_start_x = sx;
				down_start_y = sy;
				dragging = false;
			} else if (touch_down && !present) {
				touch_down = false;
				dragging = false;
				hist_x.clear();
				hist_y.clear();
				have_filtered = false;
			}
		}

		if (touch_down && !dragging) {
			int moved2 = dist2(down_start_x, down_start_y, sx, sy);
			if (moved2 >= drag_start2) {
				dragging = true;
				std::cout << "[GESTURE] DRAG_START X: " << down_start_x << " Y: " << down_start_y << "\n";
			}
		}

		int pre_fx = sx;
		int pre_fy = sy;

		// Only filter when touch is down
		if (touch_down) {
			if (adv.max_delta_px > 0 && have_filtered) {
				// Limit per-sample jump size to suppress spikes without freezing on fast motion.
				int dx = pre_fx - filt_x;
				int dy = pre_fy - filt_y;
				if (dx > adv.max_delta_px) pre_fx = filt_x + adv.max_delta_px;
				else if (dx < -adv.max_delta_px) pre_fx = filt_x - adv.max_delta_px;
				if (dy > adv.max_delta_px) pre_fy = filt_y + adv.max_delta_px;
				else if (dy < -adv.max_delta_px) pre_fy = filt_y - adv.max_delta_px;
			}
			if (adv.median_window == 3 || adv.median_window == 5) {
				int mw = adv.median_window;
				hist_x.push_back(pre_fx);
				hist_y.push_back(pre_fy);
				while ((int)hist_x.size() > mw) hist_x.pop_front();
				while ((int)hist_y.size() > mw) hist_y.pop_front();
				pre_fx = median_of(hist_x);
				pre_fy = median_of(hist_y);
			}
			if (adv.iir_alpha > 0.0f) {
				if (!have_filtered) {
					filt_x = pre_fx;
					filt_y = pre_fy;
					have_filtered = true;
				} else {
					filt_x = (int)std::llround(adv.iir_alpha * pre_fx + (1.0f - adv.iir_alpha) * filt_x);
					filt_y = (int)std::llround(adv.iir_alpha * pre_fy + (1.0f - adv.iir_alpha) * filt_y);
				}
			} else {
				filt_x = pre_fx;
				filt_y = pre_fy;
				have_filtered = true;
			}
		} else {
			// If not touching, keep last filtered but do not update
			if (!have_filtered) {
				filt_x = pre_fx;
				filt_y = pre_fy;
			}
		}

		int out_x = clamp_val(filt_x, 0, adv.screen_w - 1);
		int out_y = clamp_val(filt_y, 0, adv.screen_h - 1);

		// Detect saturated/extreme values (likely wrong CS or wiring)
		// Only warn while actually touching; otherwise XPT2046 can legitimately float/extreme.
		if (touch_down) {
			bool extreme = ((raw_x <= 0 || raw_x >= 4095) && (raw_y <= 0 || raw_y >= 4095));
			extreme_count = extreme ? (extreme_count + 1) : 0;
			if (!warned_dead && extreme_count > 30) {
				std::cerr << "[WARN] Readings are saturated (0 or 4095). Possibly wrong CS/device. Recommendation: check "
				          << (best_spi ? best_spi : "<unknown>") << " and CE wiring." << std::endl;
				warned_dead = true;
			}
		} else {
			extreme_count = 0;
		}

		// Detect static readings (no change) only while touching
		if (touch_down) {
			if (out_x == last_x && out_y == last_y) same_count++; else same_count = 0;
			last_x = out_x; last_y = out_y;
			if (!warned_static && same_count > 100) {
				std::cerr << "[INFO] Readings are static. If unexpected, check CS and touch wiring." << std::endl;
				warned_static = true;
			}
		} else {
			same_count = 0;
			last_x = out_x;
			last_y = out_y;
		}

		std::cout << "[SPI] XPT2046 X: " << x << "  Y: " << y
				  << "  (raw X: " << raw_x << " raw Y: " << raw_y
				  << " SX: " << out_x << " SY: " << out_y
				  << " Z: " << pressure << " DOWN: " << (touch_down ? 1 : 0) << ")" << std::endl;
		if (advanced_raw) {
			std::cout << "[ADV] raw(" << raw_x << "," << raw_y << ")"
					  << " z1=" << z1 << " z2=" << z2 << " pressure=" << pressure
					  << " swapped/inverted/clamped(" << x << "," << y << ")"
					  << " screen(" << sx << "," << sy << ")"
					  << " pre_filter(" << pre_fx << "," << pre_fy << ")"
					  << " filtered(" << out_x << "," << out_y << ")"
					  << " down=" << (touch_down ? 1 : 0)
					  << " dragging=" << (dragging ? 1 : 0)
					  << "\n";
		}
		fflush(stdout);
		usleep((useconds_t)adv.poll_us);
	}
	close(best_fd);
	return 0;
}

