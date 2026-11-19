// Analysis.hpp
// Loads a folder of one-shot samples, measures how each one sounds and lays
// them out on a 2D map (0..1 on both axes). No VCV Rack dependencies, so it
// can be tested on its own (see test/map_test.cpp).
//
// Measurements per sample (taken from the first moments after the hit):
//   brightness  spectral centroid, in octaves
//   noisiness   spectral flatness (0 = pure tone, 1 = white noise)
//   attack      time from the start of the hit to its loudest point
//   length      time until it has died away by 40 dB
//   weight      share of energy below 150 Hz (kicks and toms are high)
//
// Two layouts are made:
//   FIXED  X = brightness, Y = length (predictable to perform with)
//   AUTO   the two strongest directions of difference across all five
//          measurements in this folder (principal components), turned so
//          that brighter still tends right and longer tends up
// Both are evened out so the dots fill the square, then nudged apart so no
// two sit on top of each other.
#pragma once
#include "SampleFile.hpp"
#include <array>
#include <atomic>
#include <complex>
#include <functional>

namespace stella {

static const int NF = 5;
enum { F_BRIGHT, F_NOISE, F_ATTACK, F_LENGTH, F_WEIGHT };
static const double MAX_SECONDS = 2.0;      // one-shots: anything longer is trimmed
static const size_t MAX_FILES = 1000;
static const size_t MAX_TOTAL_FRAMES = 48 * 1000 * 1000;  // ~190 MB of audio

struct Entry {
	std::string path, name;
	std::vector<float> data;   // mono, at the file's own rate, trimmed, faded, normalised
	int rate = 48000;
	float feat[NF] = {};
	float ax = 0.5f, ay = 0.5f;  // AUTO position, 0..1, y up
	float fx = 0.5f, fy = 0.5f;  // FIXED position
	float colour = 0.f;          // 0 = low/heavy (warm) .. 1 = bright/noisy (cool)
	float x(bool fixed) const { return fixed ? fx : ax; }
	float y(bool fixed) const { return fixed ? fy : ay; }
};

struct Library {
	std::string folder;
	std::vector<Entry> entries;
	std::string note;   // e.g. "skipped 3 files" or why loading stopped
};

// ---- small FFT ---------------------------------------------------------------
inline void fft(std::vector<std::complex<float>>& a) {
	size_t n = a.size();
	for (size_t i = 1, j = 0; i < n; i++) {
		size_t bit = n >> 1;
		for (; j & bit; bit >>= 1) j ^= bit;
		j ^= bit;
		if (i < j) std::swap(a[i], a[j]);
	}
	for (size_t len = 2; len <= n; len <<= 1) {
		float ang = -2.f * (float) M_PI / len;
		std::complex<float> wl(std::cos(ang), std::sin(ang));
		for (size_t i = 0; i < n; i += len) {
			std::complex<float> w(1.f, 0.f);
			for (size_t k = 0; k < len / 2; k++) {
				auto u = a[i + k], v = a[i + k + len / 2] * w;
				a[i + k] = u + v;
				a[i + k + len / 2] = u - v;
				w *= wl;
			}
		}
	}
}

// ---- measuring one sample ----------------------------------------------------
inline void measure(Entry& e) {
	const std::vector<float>& x = e.data;
	int n = (int) x.size();
	double sr = e.rate;
	// loudness envelope in 5 ms windows
	int win = std::max(1, (int) (sr * 0.005));
	std::vector<float> env;
	for (int s = 0; s < n; s += win) {
		double acc = 0;
		int c = 0;
		for (int i = s; i < std::min(n, s + win); i++, c++) acc += (double) x[i] * x[i];
		env.push_back((float) std::sqrt(acc / std::max(1, c)));
	}
	int ip = 0;
	for (int i = 1; i < (int) env.size(); i++) if (env[i] > env[ip]) ip = i;
	float peak = std::max(env[ip], 1e-9f);
	int onset = 0;
	while (onset < ip && env[onset] < 0.1f * peak) onset++;
	int top = onset;
	while (top < ip && env[top] < 0.9f * peak) top++;
	int last = (int) env.size() - 1;
	while (last > ip && env[last] < 0.01f * peak) last--;
	double attack = std::max(0.0005, (top - onset) * (double) win / sr);
	double length = std::max(0.005, (last + 1 - onset) * (double) win / sr);

	// average spectrum over up to 8 frames from the onset, weighted by loudness
	const int N = 2048;
	std::vector<double> power(N / 2, 0.0);
	std::vector<std::complex<float>> buf(N);
	int start = onset * win;
	int hop = N / 2;
	for (int f = 0; f < 8; f++) {
		int s = start + f * hop;
		if (s >= n) break;
		for (int i = 0; i < N; i++) {
			float w = 0.5f - 0.5f * std::cos(2.f * (float) M_PI * i / (N - 1));
			buf[i] = (s + i < n) ? x[s + i] * w : 0.f;
		}
		fft(buf);
		for (int k = 0; k < N / 2; k++) power[k] += std::norm(buf[k]);
	}
	double binHz = sr / N;
	double tot = 0, cen = 0, low = 0, logSum = 0;
	int cnt = 0;
	for (int k = 1; k < N / 2; k++) {
		double hz = k * binHz;
		if (hz < 20 || hz > 18000) continue;
		double p = power[k] + 1e-12;
		tot += p;
		cen += p * hz;
		if (hz < 150) low += p;
		logSum += std::log(p);
		cnt++;
	}
	double centroid = tot > 0 ? cen / tot : 1000;
	double flat = (cnt > 0 && tot > 0) ? std::exp(logSum / cnt) / (tot / cnt) : 0;
	e.feat[F_BRIGHT] = (float) std::log2(std::max(40.0, centroid));
	e.feat[F_NOISE] = (float) std::pow(std::min(1.0, flat), 0.25);  // spread out the low end
	e.feat[F_ATTACK] = (float) std::log2(attack);
	e.feat[F_LENGTH] = (float) std::log2(length);
	e.feat[F_WEIGHT] = (float) (tot > 0 ? low / tot : 0);
}

// ---- turning measurements into map positions ---------------------------------
// Spreads values evenly over 0..1: part rank order (fills the square), part
// straight scaling (keeps real gaps visible).
inline std::vector<float> spread01(const std::vector<double>& v) {
	size_t n = v.size();
	std::vector<float> out(n, 0.5f);
	if (n < 2) return out;
	std::vector<size_t> idx(n);
	for (size_t i = 0; i < n; i++) idx[i] = i;
	std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return v[a] < v[b]; });
	double lo = v[idx[0]], hi = v[idx[n - 1]];
	for (size_t r = 0; r < n; r++) {
		size_t i = idx[r];
		double rank = (double) r / (n - 1);
		double lin = hi > lo ? (v[i] - lo) / (hi - lo) : 0.5;
		out[i] = (float) (0.6 * rank + 0.4 * lin);
	}
	return out;
}

// Pushes dots that sit almost on top of each other apart a little.
inline void relax(std::vector<float>& xs, std::vector<float>& ys) {
	size_t n = xs.size();
	if (n < 2) return;
	float dmin = std::min(0.03f, 0.7f / std::sqrt((float) n));
	for (int it = 0; it < 24; it++) {
		bool moved = false;
		for (size_t i = 0; i < n; i++)
			for (size_t j = i + 1; j < n; j++) {
				float dx = xs[j] - xs[i], dy = ys[j] - ys[i];
				float d2 = dx * dx + dy * dy;
				if (d2 >= dmin * dmin) continue;
				float d = std::sqrt(d2);
				if (d < 1e-6f) {  // identical: separate in a fixed direction
					float a = (float) ((i * 7 + j * 13) % 17) / 17.f * 6.2832f;
					dx = std::cos(a); dy = std::sin(a); d = 1.f;
					d2 = 0;
				}
				float push = (dmin - std::sqrt(d2)) * 0.5f / d;
				xs[i] -= dx * push; ys[i] -= dy * push;
				xs[j] += dx * push; ys[j] += dy * push;
				moved = true;
			}
		for (size_t i = 0; i < n; i++) {
			xs[i] = std::min(0.98f, std::max(0.02f, xs[i]));
			ys[i] = std::min(0.98f, std::max(0.02f, ys[i]));
		}
		if (!moved) break;
	}
}

inline void layout(Library& lib) {
	size_t n = lib.entries.size();
	if (n == 0) return;
	// standardise each measurement
	double mean[NF] = {}, sd[NF] = {};
	for (auto& e : lib.entries) for (int f = 0; f < NF; f++) mean[f] += e.feat[f];
	for (int f = 0; f < NF; f++) mean[f] /= n;
	for (auto& e : lib.entries) for (int f = 0; f < NF; f++) sd[f] += (e.feat[f] - mean[f]) * (e.feat[f] - mean[f]);
	for (int f = 0; f < NF; f++) sd[f] = std::sqrt(sd[f] / n) + 1e-9;
	const double weight[NF] = {1.2, 1.0, 0.6, 1.0, 1.0};
	std::vector<std::array<double, NF>> z(n);
	for (size_t i = 0; i < n; i++)
		for (int f = 0; f < NF; f++) z[i][f] = (lib.entries[i].feat[f] - mean[f]) / sd[f] * weight[f];

	// FIXED layout
	std::vector<double> bx(n), by(n);
	for (size_t i = 0; i < n; i++) { bx[i] = lib.entries[i].feat[F_BRIGHT]; by[i] = lib.entries[i].feat[F_LENGTH]; }
	std::vector<float> fx = spread01(bx), fy = spread01(by);

	// AUTO layout: two principal components by power iteration
	double C[NF][NF] = {};
	for (size_t i = 0; i < n; i++)
		for (int a = 0; a < NF; a++)
			for (int b = 0; b < NF; b++) C[a][b] += z[i][a] * z[i][b] / n;
	double pc[2][NF];
	for (int k = 0; k < 2; k++) {
		double v[NF];
		for (int f = 0; f < NF; f++) v[f] = 1.0 + 0.1 * f * (k + 1);
		for (int it = 0; it < 200; it++) {
			double w[NF] = {};
			for (int a = 0; a < NF; a++) for (int b = 0; b < NF; b++) w[a] += C[a][b] * v[b];
			if (k == 1) {  // keep it at right angles to the first
				double d = 0;
				for (int f = 0; f < NF; f++) d += w[f] * pc[0][f];
				for (int f = 0; f < NF; f++) w[f] -= d * pc[0][f];
			}
			double norm = 0;
			for (int f = 0; f < NF; f++) norm += w[f] * w[f];
			norm = std::sqrt(norm) + 1e-12;
			for (int f = 0; f < NF; f++) v[f] = w[f] / norm;
		}
		for (int f = 0; f < NF; f++) pc[k][f] = v[f];
	}
	std::vector<double> px(n), py(n);
	for (size_t i = 0; i < n; i++) {
		px[i] = py[i] = 0;
		for (int f = 0; f < NF; f++) { px[i] += z[i][f] * pc[0][f]; py[i] += z[i][f] * pc[1][f]; }
	}
	// turn the axes so brighter leans right and longer leans up, as in FIXED
	auto corr = [&](const std::vector<double>& p, int feat) {
		double s = 0;
		for (size_t i = 0; i < n; i++) s += p[i] * z[i][feat];
		return s;
	};
	if (std::fabs(corr(py, F_BRIGHT)) > std::fabs(corr(px, F_BRIGHT)) &&
	    std::fabs(corr(px, F_LENGTH)) > std::fabs(corr(py, F_LENGTH))) std::swap(px, py);
	if (corr(px, F_BRIGHT) < 0) for (auto& v : px) v = -v;
	if (corr(py, F_LENGTH) < 0) for (auto& v : py) v = -v;
	std::vector<float> ax = spread01(px), ay = spread01(py);

	if (n <= 1500) { relax(fx, fy); relax(ax, ay); }

	// dot colour: heavy/low = warm, bright/noisy = cool
	std::vector<double> col(n);
	for (size_t i = 0; i < n; i++) col[i] = z[i][F_BRIGHT] + z[i][F_NOISE] - 1.5 * z[i][F_WEIGHT];
	std::vector<float> c01 = spread01(col);
	for (size_t i = 0; i < n; i++) {
		Entry& e = lib.entries[i];
		e.fx = fx[i]; e.fy = fy[i];
		e.ax = ax[i]; e.ay = ay[i];
		e.colour = c01[i];
	}
}

// ---- loading a folder --------------------------------------------------------
inline bool isAudioName(const std::string& fn) {
	std::string l = fn;
	for (auto& c : l) c = (char) std::tolower((unsigned char) c);
	auto ends = [&](const char* s) { size_t k = std::strlen(s); return l.size() >= k && l.compare(l.size() - k, k, s) == 0; };
	return ends(".wav") || ends(".wave") || ends(".aif") || ends(".aiff") || ends(".aifc");
}

// files: full paths (already sorted). progress(done, total) is called as it goes;
// abort() returning true stops early.
inline Library* loadFiles(const std::string& folder, const std::vector<std::string>& files, bool normalise,
                          std::function<void(int, int)> progress, std::function<bool()> abort) {
	Library* lib = new Library;
	lib->folder = folder;
	size_t frames = 0;
	int skipped = 0;
	int total = (int) std::min(files.size(), MAX_FILES);
	for (int i = 0; i < total; i++) {
		if (abort && abort()) break;
		if (progress) progress(i, total);
		tp::Sample s;
		std::string err = tp::readSample(files[i], s, MAX_SECONDS);
		if (!err.empty()) { skipped++; continue; }
		Entry e;
		e.path = files[i];
		std::string fn = files[i].substr(files[i].find_last_of("/\\") + 1);
		size_t dot = fn.find_last_of('.');
		e.name = dot == std::string::npos ? fn : fn.substr(0, dot);
		e.rate = s.rate;
		int nf = s.frames();
		e.data.resize(nf);
		float pk = 0;
		for (int k = 0; k < nf; k++) {
			float v = s.R.empty() ? s.L[k] : 0.5f * (s.L[k] + s.R[k]);
			e.data[k] = v;
			pk = std::max(pk, std::fabs(v));
		}
		if (pk < 1e-5f) { skipped++; continue; }  // silent file
		// trimmed at 2 s: fade the last 30 ms so it doesn't click
		if (nf >= (int) (MAX_SECONDS * s.rate) - 1) {
			int fl = std::min(nf, (int) (0.03 * s.rate));
			for (int k = 0; k < fl; k++) e.data[nf - 1 - k] *= (float) k / fl;
		}
		if (normalise) for (auto& v : e.data) v /= pk;
		measure(e);
		frames += nf;
		lib->entries.push_back(std::move(e));
		if (frames > MAX_TOTAL_FRAMES) { lib->note = "stopped at " + std::to_string(lib->entries.size()) + " samples (memory)"; break; }
	}
	if (files.size() > MAX_FILES && lib->note.empty()) lib->note = "first " + std::to_string(MAX_FILES) + " files only";
	if (skipped && lib->note.empty()) lib->note = "skipped " + std::to_string(skipped) + " unreadable";
	layout(*lib);
	if (progress) progress(total, total);
	return lib;
}

// ---- choosing what to play ---------------------------------------------------
inline int nearest(const Library& lib, bool fixed, float x, float y) {
	int best = -1;
	float bd = 1e9f;
	for (int i = 0; i < (int) lib.entries.size(); i++) {
		float dx = lib.entries[i].x(fixed) - x, dy = lib.entries[i].y(fixed) - y;
		float d = dx * dx + dy * dy;
		if (d < bd) { bd = d; best = i; }
	}
	return best;
}

// SPREAD 0..1 to a ring radius on the map
inline float radiusFor(float spread) { return 0.02f + 0.38f * spread; }

// With probability `chance`, a sample inside the ring other than the nearest
// is played instead; closer ones are more likely. r01a, r01b: random 0..1.
inline int pick(const Library& lib, bool fixed, float x, float y, float radius, float chance, float r01a, float r01b) {
	int n0 = nearest(lib, fixed, x, y);
	if (n0 < 0 || r01a >= chance) return n0;
	double total = 0;
	int count = (int) lib.entries.size();
	auto weightOf = [&](int i) -> double {
		if (i == n0) return 0;
		float dx = lib.entries[i].x(fixed) - x, dy = lib.entries[i].y(fixed) - y;
		float d = std::sqrt(dx * dx + dy * dy);
		if (d >= radius) return 0;
		double w = 1.0 - d / radius;
		return w * w + 0.02;
	};
	for (int i = 0; i < count; i++) total += weightOf(i);
	if (total <= 0) return n0;
	double t = r01b * total;
	for (int i = 0; i < count; i++) {
		double w = weightOf(i);
		if (w <= 0) continue;
		if (t < w) return i;
		t -= w;
	}
	return n0;
}

} // namespace stella
