#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/spi/spidev.h>
#include <linux/uinput.h>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

static std::atomic<bool> g_running{true};

static void handle_signal(int) {
	g_running = false;
}

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
	// System-wide config (useful when running as a system service)
	candidates.push_back("/etc/xpt2046/touch_config.txt");
	candidates.push_back("touch_config.txt");
	candidates.push_back("installation/touch_config.txt");

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
	int poll_us = 100000;

	int offset_x = 0;
	int offset_y = 0;
	float scale_x = 1.0f;
	float scale_y = 1.0f;

	int deadzone_left = 0;
	int deadzone_right = 0;
	int deadzone_top = 0;
	int deadzone_bottom = 0;

	int median_window = 3;
	float iir_alpha = 0.20f;

	int press_threshold = 120;
	int release_threshold = 80;

	int max_delta_px = 0;
};

static void sanitize_adv(AdvancedParams& adv) {
	adv.screen_w = clamp_val(adv.screen_w, 1, 4096);
	adv.screen_h = clamp_val(adv.screen_h, 1, 4096);
	adv.poll_us = clamp_val(adv.poll_us, 1000, 1000000);
	adv.scale_x = clamp_val(adv.scale_x, 0.01f, 10.0f);
	adv.scale_y = clamp_val(adv.scale_y, 0.01f, 10.0f);
	adv.iir_alpha = clamp_val(adv.iir_alpha, 0.0f, 1.0f);
	if (!(adv.median_window == 0 || adv.median_window == 3 || adv.median_window == 5)) adv.median_window = 3;
	if (adv.release_threshold > adv.press_threshold) adv.release_threshold = adv.press_threshold;
}

static bool stat_mtime(const std::string& path, timespec& out) {
	struct stat st;
	if (path.empty()) return false;
	if (stat(path.c_str(), &st) != 0) return false;
	out = st.st_mtim;
	return true;
}

static bool timespec_differs(const timespec& a, const timespec& b) {
	return a.tv_sec != b.tv_sec || a.tv_nsec != b.tv_nsec;
}

static void load_config(int& invert_x,
						int& invert_y,
						int& swap_xy,
						int& min_x,
						int& max_x,
						int& min_y,
						int& max_y,
						AdvancedParams& adv,
						std::string& usedPath,
						std::string& spi_device_cfg) {
	invert_x = invert_y = swap_xy = 0;
	min_x = min_y = 0;
	max_x = max_y = 4095;

	usedPath = find_config_path();
	if (usedPath.empty()) return;

	std::ifstream file(usedPath);
	std::string line;
	while (std::getline(file, line)) {
		if (line.empty() || line[0] == '#') continue;
		auto eq = line.find('=');
		if (eq == std::string::npos) continue;
		std::string key = line.substr(0, eq);
		std::string val = line.substr(eq + 1);
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
	}
}

static void apply_env_overrides(int& invert_x,
							int& invert_y,
							int& swap_xy,
							int& min_x,
							int& max_x,
							int& min_y,
							int& max_y,
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

	if (const char* v = getenv("XPT_SPI_DEVICE")) {
		if (*v) spi_device_cfg = v;
	}

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
}

static int read_xpt2046(int spi_fd, uint8_t command) {
	uint8_t tx[3] = {command, 0x00, 0x00};
	uint8_t rx[3] = {0};
	struct spi_ioc_transfer tr = {};
	tr.tx_buf = (unsigned long)tx;
	tr.rx_buf = (unsigned long)rx;
	tr.len = 3;
	tr.speed_hz = 1000000;
	tr.bits_per_word = 8;
	tr.delay_usecs = 0;
	int ret = ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tr);
	if (ret < 1) return -1;
	int value = ((rx[1] << 5) | (rx[2] >> 3)) & 0xFFF;
	return value;
}

static int open_spi_best(const std::string& spi_device_cfg, std::string& used_device) {
	std::vector<std::string> candidates;
	if (!spi_device_cfg.empty()) candidates.push_back(spi_device_cfg);
	candidates.push_back("/dev/spidev0.1");
	candidates.push_back("/dev/spidev0.0");
	candidates.push_back("/dev/spidev1.0");
	candidates.push_back("/dev/spidev1.1");

	uint8_t mode = SPI_MODE_0;
	uint32_t speed = 1000000;

	for (const auto& dev : candidates) {
		int fd = open(dev.c_str(), O_RDWR);
		if (fd < 0) continue;
		if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0) {
			close(fd);
			continue;
		}
		if (ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
			close(fd);
			continue;
		}
		used_device = dev;
		return fd;
	}
	return -1;
}

static int uinput_create_touch(int screen_w, int screen_h) {
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (fd < 0) {
		std::perror("open(/dev/uinput)");
		return -1;
	}

	const int max_x = std::max(0, screen_w - 1);
	const int max_y = std::max(0, screen_h - 1);

	// Mark as a direct touch device (not a touchpad)
	(void)ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

	if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) goto fail;
	if (ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH) < 0) goto fail;
	// Some stacks expect TOOL_FINGER for touchscreens
	(void)ioctl(fd, UI_SET_KEYBIT, BTN_TOOL_FINGER);
	if (ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0) goto fail;
	if (ioctl(fd, UI_SET_ABSBIT, ABS_X) < 0) goto fail;
	if (ioctl(fd, UI_SET_ABSBIT, ABS_Y) < 0) goto fail;

	// Multitouch-style reporting (works well with SDL/Qt/evdev)
	(void)ioctl(fd, UI_SET_ABSBIT, ABS_MT_SLOT);
	(void)ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
	(void)ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
	(void)ioctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
	(void)ioctl(fd, UI_SET_ABSBIT, ABS_MT_PRESSURE);

	if (ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) goto fail;

	uinput_user_dev uidev;
	std::memset(&uidev, 0, sizeof(uidev));
	std::snprintf(uidev.name, sizeof(uidev.name), "XPT2046 uinput touch");
	uidev.id.bustype = BUS_USB;
	uidev.id.vendor = 0x1234;
	uidev.id.product = 0x5678;
	uidev.id.version = 1;

	uidev.absmin[ABS_X] = 0;
	uidev.absmax[ABS_X] = max_x;
	uidev.absmin[ABS_Y] = 0;
	uidev.absmax[ABS_Y] = max_y;

	uidev.absmin[ABS_MT_SLOT] = 0;
	uidev.absmax[ABS_MT_SLOT] = 0;

	uidev.absmin[ABS_MT_POSITION_X] = 0;
	uidev.absmax[ABS_MT_POSITION_X] = max_x;
	uidev.absmin[ABS_MT_POSITION_Y] = 0;
	uidev.absmax[ABS_MT_POSITION_Y] = max_y;
	uidev.absmin[ABS_MT_TRACKING_ID] = 0;
	uidev.absmax[ABS_MT_TRACKING_ID] = 65535;
	uidev.absmin[ABS_MT_PRESSURE] = 0;
	uidev.absmax[ABS_MT_PRESSURE] = 4095;

	if (write(fd, &uidev, sizeof(uidev)) < 0) goto fail;
	if (ioctl(fd, UI_DEV_CREATE) < 0) goto fail;

	// Give the input subsystem a moment
	usleep(100000);
	return fd;

fail:
	std::perror("uinput setup");
	close(fd);
	return -1;
}

static void uinput_emit(int fd, uint16_t type, uint16_t code, int32_t value) {
	input_event ev;
	std::memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.code = code;
	ev.value = value;
	// time left as 0; kernel fills / not required
	(void)write(fd, &ev, sizeof(ev));
}

static void uinput_sync(int fd) {
	uinput_emit(fd, EV_SYN, SYN_REPORT, 0);
}


int main() {
	std::signal(SIGINT, handle_signal);
	std::signal(SIGTERM, handle_signal);

	int invert_x = 0, invert_y = 0, swap_xy = 0;
	int min_x = 0, max_x = 4095, min_y = 0, max_y = 4095;
	AdvancedParams adv;
	std::string cfgPath;
	std::string spi_device_cfg;
	load_config(invert_x, invert_y, swap_xy, min_x, max_x, min_y, max_y, adv, cfgPath, spi_device_cfg);
	apply_env_overrides(invert_x, invert_y, swap_xy, min_x, max_x, min_y, max_y, adv, spi_device_cfg);
	sanitize_adv(adv);

	std::string used_spi;
	int spi_fd = open_spi_best(spi_device_cfg, used_spi);
	if (spi_fd < 0) {
		std::cerr << "[ERROR] Failed to open any SPI device (spidev)." << std::endl;
		return 1;
	}

	int ui_fd = uinput_create_touch(adv.screen_w, adv.screen_h);
	if (ui_fd < 0) {
		close(spi_fd);
		return 1;
	}

	std::cerr << "[INFO] xpt2046_uinputd started. cfg=" << (cfgPath.empty() ? "<none>" : cfgPath)
			  << " spi=" << used_spi
			  << " screen=" << adv.screen_w << "x" << adv.screen_h
			  << " poll_us=" << adv.poll_us
			  << " active_poll_us=" << 5000
			  << std::endl;

	timespec cfg_mtime{0, 0};
	(void)stat_mtime(cfgPath, cfg_mtime);
	auto next_cfg_check = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);

	bool touch_down = false;
	int32_t tracking_id = 1;
	int press_streak = 0;
	int release_streak = 0;
	int candidate_x = 0;
	int candidate_y = 0;
	bool have_candidate = false;
	bool have_filtered = false;
	int filt_x = 0;
	int filt_y = 0;
	std::deque<int> hist_x;
	std::deque<int> hist_y;

	auto median_of = [](std::deque<int> v) -> int {
		if (v.empty()) return 0;
		std::sort(v.begin(), v.end());
		return v[v.size() / 2];
	};

	while (g_running) {
		// Auto-reload config when touch_config.txt changes. We only reload while idle
		// to avoid mid-gesture jumps.
		const auto now = std::chrono::steady_clock::now();
		if (now >= next_cfg_check) {
			next_cfg_check = now + std::chrono::milliseconds(500);
			if (!touch_down) {
				// Only attempt reload while not touching; pressure_touch is known only after we read Z.
				timespec new_mtime{0, 0};
				std::string newPath = find_config_path();
				if (!newPath.empty() && (newPath != cfgPath || (stat_mtime(newPath, new_mtime) && timespec_differs(new_mtime, cfg_mtime)))) {
					int new_invert_x = 0, new_invert_y = 0, new_swap_xy = 0;
					int new_min_x = 0, new_max_x = 4095, new_min_y = 0, new_max_y = 4095;
					AdvancedParams new_adv;
					std::string usedPath;
					std::string new_spi_device_cfg;
					load_config(new_invert_x, new_invert_y, new_swap_xy, new_min_x, new_max_x, new_min_y, new_max_y, new_adv, usedPath, new_spi_device_cfg);
					apply_env_overrides(new_invert_x, new_invert_y, new_swap_xy, new_min_x, new_max_x, new_min_y, new_max_y, new_adv, new_spi_device_cfg);
					sanitize_adv(new_adv);

					invert_x = new_invert_x;
					invert_y = new_invert_y;
					swap_xy = new_swap_xy;
					min_x = new_min_x;
					max_x = new_max_x;
					min_y = new_min_y;
					max_y = new_max_y;
					adv = new_adv;
					cfgPath = usedPath;
					spi_device_cfg = new_spi_device_cfg;

					(void)stat_mtime(cfgPath, cfg_mtime);

					// Reset filters so new config takes effect cleanly.
					press_streak = 0;
					release_streak = 0;
					have_filtered = false;
					hist_x.clear();
					hist_y.clear();
					have_candidate = false;

					std::cerr << "[INFO] Reloaded cfg=" << (cfgPath.empty() ? "<none>" : cfgPath)
							  << " poll_us=" << adv.poll_us
							  << " iir_alpha=" << adv.iir_alpha
							  << " median_window=" << adv.median_window
							  << " press_threshold=" << adv.press_threshold
							  << " release_threshold=" << adv.release_threshold
							  << std::endl;
				}
			}
		}

		// Fast update while finger is down to keep UI responsive.
		// Also switch to fast mode immediately when pressure suggests a touch (even before debounce)
		// so the first movement is not delayed by a long idle poll.
		const int active_poll_us = 5000; // 200 Hz when touching

		int raw_x = read_xpt2046(spi_fd, 0x90);
		int raw_y = read_xpt2046(spi_fd, 0xD0);
		int z1 = read_xpt2046(spi_fd, 0xB0);
		int pressure = (z1 >= 0) ? z1 : 0;

		const bool pressure_touch = (adv.press_threshold > 0) ? (pressure >= adv.press_threshold) : (pressure > 0);
		const int sleep_us = (touch_down || pressure_touch) ? active_poll_us : adv.poll_us;
		if (raw_x < 0 || raw_y < 0) {
			usleep((useconds_t)sleep_us);
			continue;
		}

		// While not touching, do NOT update filters from floating/noise samples.
		// Otherwise the filtered state drifts to a corner and the first real touch "travels" from there.
		if (!touch_down && !pressure_touch) {
			press_streak = 0;
			release_streak = 0;
			have_filtered = false;
			hist_x.clear();
			hist_y.clear();
			have_candidate = false;
			usleep((useconds_t)sleep_us);
			continue;
		}

		int x = raw_x;
		int y = raw_y;
		if (swap_xy) std::swap(x, y);
		if (invert_x) x = 4095 - x;
		if (invert_y) y = 4095 - y;
		x = clamp_val(x, min_x, max_x);
		y = clamp_val(y, min_y, max_y);

		int sx = (x - min_x) * (adv.screen_w - 1) / std::max(1, (max_x - min_x));
		int sy = (y - min_y) * (adv.screen_h - 1) / std::max(1, (max_y - min_y));
		sx = clamp_val(sx, 0, adv.screen_w - 1);
		sy = clamp_val(sy, 0, adv.screen_h - 1);

		sx = (int)std::llround(sx * adv.scale_x + adv.offset_x);
		sy = (int)std::llround(sy * adv.scale_y + adv.offset_y);

		int min_sx = clamp_val(adv.deadzone_left, 0, adv.screen_w - 1);
		int max_sx = clamp_val(adv.screen_w - 1 - adv.deadzone_right, 0, adv.screen_w - 1);
		int min_sy = clamp_val(adv.deadzone_top, 0, adv.screen_h - 1);
		int max_sy = clamp_val(adv.screen_h - 1 - adv.deadzone_bottom, 0, adv.screen_h - 1);
		if (max_sx < min_sx) max_sx = min_sx;
		if (max_sy < min_sy) max_sy = min_sy;
		sx = clamp_val(sx, min_sx, max_sx);
		sy = clamp_val(sy, min_sy, max_sy);

		// Remember the raw mapped position for instant touch-down.
		candidate_x = sx;
		candidate_y = sy;
		have_candidate = true;

		// Touch state with hysteresis + debounce (prevents rapid DOWN/UP chatter)
		if (adv.press_threshold > 0) {
			if (!touch_down) {
				if (pressure >= adv.press_threshold) {
					press_streak++;
					if (press_streak >= 2) {
						touch_down = true;
						// Reset filters so first reported position snaps to finger.
						have_filtered = false;
						hist_x.clear();
						hist_y.clear();
						if (have_candidate) {
							filt_x = candidate_x;
							filt_y = candidate_y;
							have_filtered = true;
						}
						release_streak = 0;
					}
				} else {
					press_streak = 0;
				}
			} else {
				press_streak = 0;
				if (pressure <= adv.release_threshold) {
					release_streak++;
					if (release_streak >= 2) {
						touch_down = false;
						have_filtered = false;
						hist_x.clear();
						hist_y.clear();
						have_candidate = false;
						release_streak = 0;
					}
				} else {
					release_streak = 0;
				}
			}
		} else {
			// No thresholds: fallback to simple pressure heuristic but still debounce.
			if (!touch_down) {
				if (pressure > 0) {
					press_streak++;
					if (press_streak >= 2) {
						touch_down = true;
						have_filtered = false;
						hist_x.clear();
						hist_y.clear();
						if (have_candidate) {
							filt_x = candidate_x;
							filt_y = candidate_y;
							have_filtered = true;
						}
						release_streak = 0;
					}
				} else {
					press_streak = 0;
				}
			} else {
				press_streak = 0;
				if (pressure <= 0) {
					release_streak++;
					if (release_streak >= 2) {
						touch_down = false;
						have_filtered = false;
						hist_x.clear();
						hist_y.clear();
						have_candidate = false;
						release_streak = 0;
					}
				} else {
					release_streak = 0;
				}
			}
		}

		// If we're still debouncing (pressure detected but not yet touch_down), don't run filters.
		if (!touch_down) {
			usleep((useconds_t)sleep_us);
			continue;
		}

		int out_x = sx;
		int out_y = sy;

		// Clamp step (max_delta_px)
		if (adv.max_delta_px > 0 && have_filtered) {
			int dx = out_x - filt_x;
			int dy = out_y - filt_y;
			if (dx > adv.max_delta_px) out_x = filt_x + adv.max_delta_px;
			else if (dx < -adv.max_delta_px) out_x = filt_x - adv.max_delta_px;
			if (dy > adv.max_delta_px) out_y = filt_y + adv.max_delta_px;
			else if (dy < -adv.max_delta_px) out_y = filt_y - adv.max_delta_px;
		}

		// Median filter
		if (adv.median_window == 3 || adv.median_window == 5) {
			int mw = adv.median_window;
			hist_x.push_back(out_x);
			hist_y.push_back(out_y);
			while ((int)hist_x.size() > mw) hist_x.pop_front();
			while ((int)hist_y.size() > mw) hist_y.pop_front();
			out_x = median_of(hist_x);
			out_y = median_of(hist_y);
		}

		// IIR smoothing
		if (adv.iir_alpha > 0.0f) {
			if (!have_filtered) {
				filt_x = out_x;
				filt_y = out_y;
				have_filtered = true;
			} else {
				float a = adv.iir_alpha;
				filt_x = (int)std::llround((1.0f - a) * (float)filt_x + a * (float)out_x);
				filt_y = (int)std::llround((1.0f - a) * (float)filt_y + a * (float)out_y);
			}
			out_x = filt_x;
			out_y = filt_y;
		} else {
			filt_x = out_x;
			filt_y = out_y;
			have_filtered = true;
		}

		static bool last_down = false;
		if (touch_down) {
			// Type-B MT: set slot + tracking + position first, then key.
			uinput_emit(ui_fd, EV_ABS, ABS_MT_SLOT, 0);
			if (!last_down) {
				uinput_emit(ui_fd, EV_ABS, ABS_MT_TRACKING_ID, tracking_id++);
			}
			uinput_emit(ui_fd, EV_ABS, ABS_MT_POSITION_X, out_x);
			uinput_emit(ui_fd, EV_ABS, ABS_MT_POSITION_Y, out_y);
			uinput_emit(ui_fd, EV_ABS, ABS_MT_PRESSURE, pressure);

			// Also publish single-touch ABS for compatibility.
			uinput_emit(ui_fd, EV_ABS, ABS_X, out_x);
			uinput_emit(ui_fd, EV_ABS, ABS_Y, out_y);

			if (!last_down) {
				uinput_emit(ui_fd, EV_KEY, BTN_TOUCH, 1);
				uinput_emit(ui_fd, EV_KEY, BTN_TOOL_FINGER, 1);
			}
			uinput_sync(ui_fd);
		} else {
			if (last_down) {
				// End touch contact
				uinput_emit(ui_fd, EV_ABS, ABS_MT_SLOT, 0);
				uinput_emit(ui_fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
				uinput_emit(ui_fd, EV_KEY, BTN_TOUCH, 0);
				uinput_emit(ui_fd, EV_KEY, BTN_TOOL_FINGER, 0);
				uinput_sync(ui_fd);
			}
		}
		last_down = touch_down;

		usleep((useconds_t)sleep_us);
	}

	ioctl(ui_fd, UI_DEV_DESTROY);
	close(ui_fd);
	close(spi_fd);
	std::cerr << "[INFO] xpt2046_uinputd exiting." << std::endl;
	return 0;
}
