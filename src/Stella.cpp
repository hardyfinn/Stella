// Stella: a 4-voice sample player that lays a folder of one-shots out on a
// 2D map by how they sound. Each voice has a point on the map (X/Y); a
// trigger plays the nearest sample, or, with probability CHANCE, another
// sample inside the SPREAD ring (closer ones more likely).
// DRIFT makes every point wander slowly; GRAVITY pulls a point towards each
// sample it plays.
#include "plugin.hpp"
#include "Analysis.hpp"
#include <osdialog.h>
#include <thread>
#include <mutex>

static const int NV = 4;
static const NVGcolor VOICE_COL[NV] = {
	nvgRGB(0xff, 0xb3, 0x47), nvgRGB(0x5f, 0xd3, 0xe6), nvgRGB(0xff, 0x7f, 0xb0), nvgRGB(0xb6, 0xe0, 0x5a)};

struct Player {
	const stella::Entry* e = nullptr;
	double pos = 0, inc = 1;
	float gain = 1.f, fade = 0.f;  // fade > 0: fading out by this much per sample
	float next() {
		if (!e) return 0.f;
		size_t i = (size_t) pos;
		if (i + 1 >= e->data.size()) { e = nullptr; return 0.f; }
		float t = (float) (pos - i);
		float v = e->data[i] + (e->data[i + 1] - e->data[i]) * t;
		pos += inc;
		float out = v * gain;
		if (fade > 0.f) {
			gain -= fade;
			if (gain <= 0.f) e = nullptr;
		}
		return out;
	}
};

struct Stella : Module {
	enum ParamId {
		AXIS_PARAM, LOAD_PARAM, DRIFT_PARAM, GRAVITY_PARAM,
		ENUMS(X_PARAM, NV), ENUMS(Y_PARAM, NV), ENUMS(SPREAD_PARAM, NV), ENUMS(CHANCE_PARAM, NV),
		PARAMS_LEN
	};
	enum InputId {
		DRIFT_INPUT, ENUMS(TRIG_INPUT, NV), ENUMS(X_INPUT, NV), ENUMS(Y_INPUT, NV), ENUMS(CHANCE_INPUT, NV),
		INPUTS_LEN
	};
	enum OutputId { MIX_OUTPUT, ENUMS(OUT_OUTPUT, NV), OUTPUTS_LEN };
	enum LightId { LIGHTS_LEN };

	// the library in use; swapped in from the loader thread by process()
	stella::Library* lib = nullptr;
	std::atomic<stella::Library*> pending{nullptr};
	std::atomic<stella::Library*> garbage{nullptr};  // deleted by the widget (UI thread)

	// loader
	std::thread loader;
	std::atomic<bool> abortLoad{false};
	std::atomic<bool> loading{false};
	std::atomic<int> loadDone{0}, loadTotal{0};
	std::vector<std::string> folders;  // folders on the map (saved in the patch)
	bool subfolders = true;
	bool normalise = true;

	// voices
	dsp::SchmittTrigger trig[NV];
	Player cur[NV], old[NV];
	float offX[NV] = {}, offY[NV] = {};   // drift + gravity offsets
	float driftVX[NV] = {}, driftVY[NV] = {};
	// for the screen
	float posX[NV], posY[NV], radius[NV];
	int lastHit[NV] = {-1, -1, -1, -1};
	float flash[NV] = {};
	int ctrl = 0;

	Stella() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configSwitch(AXIS_PARAM, 0.f, 1.f, 1.f, "Map axes", {"Auto (by similarity)", "Fixed (X bright, Y long)"});
		configButton(LOAD_PARAM, "Load folder");
		configParam(DRIFT_PARAM, 0.f, 1.f, 0.f, "Drift (points wander)", "%", 0.f, 100.f);
		configParam(GRAVITY_PARAM, 0.f, 1.f, 0.f, "Gravity (pull towards each played sample)", "%", 0.f, 100.f);
		configInput(DRIFT_INPUT, "Drift CV (±5 V = ±50%)");
		configOutput(MIX_OUTPUT, "Mix");
		const float startX[NV] = {0.2f, 0.75f, 0.5f, 0.8f}, startY[NV] = {0.3f, 0.75f, 0.5f, 0.25f};
		for (int v = 0; v < NV; v++) {
			std::string n = "Voice " + std::to_string(v + 1);
			configParam(X_PARAM + v, 0.f, 1.f, startX[v], n + " X", "%", 0.f, 100.f);
			configParam(Y_PARAM + v, 0.f, 1.f, startY[v], n + " Y", "%", 0.f, 100.f);
			configParam(SPREAD_PARAM + v, 0.f, 1.f, 0.25f, n + " spread (ring size)", "%", 0.f, 100.f);
			configParam(CHANCE_PARAM + v, 0.f, 1.f, 0.f, n + " chance of a nearby sample", "%", 0.f, 100.f);
			configInput(TRIG_INPUT + v, n + " trigger");
			configInput(X_INPUT + v, n + " X CV (±5 V = whole map)");
			configInput(Y_INPUT + v, n + " Y CV (±5 V = whole map)");
			configInput(CHANCE_INPUT + v, n + " chance CV (±5 V = ±50%)");
			configOutput(OUT_OUTPUT + v, n + " (patching it takes the voice out of the mix)");
			posX[v] = startX[v]; posY[v] = startY[v]; radius[v] = stella::radiusFor(0.25f);
		}
	}

	~Stella() {
		stopLoader();
		delete lib;
		delete pending.exchange(nullptr);
		delete garbage.exchange(nullptr);
	}

	void stopLoader() {
		abortLoad = true;
		if (loader.joinable()) loader.join();
		abortLoad = false;
	}

	// UI thread only
	// replaces the map with these folders
	void loadFolders(const std::vector<std::string>& dirs) {
		stopLoader();
		folders.clear();
		for (const std::string& d : dirs)
			if (!d.empty() && std::find(folders.begin(), folders.end(), d) == folders.end()) folders.push_back(d);
		if (folders.empty()) return;
		std::vector<std::string> dl = folders;
		std::string label = system::getFilename(dl[0]);
		if (dl.size() > 1) label += " +" + std::to_string(dl.size() - 1);
		bool sub = subfolders, norm = normalise;
		loading = true;
		loadDone = 0;
		loadTotal = 0;
		loader = std::thread([this, dl, label, sub, norm]() {
			std::vector<std::string> files;
			for (const std::string& d : dl) scan(d, files, sub ? 4 : 0);
			std::sort(files.begin(), files.end());
			files.erase(std::unique(files.begin(), files.end()), files.end());  // nested folders
			stella::Library* l = stella::loadFiles(label, files, norm,
				[this](int d, int t) { loadDone = d; loadTotal = t; },
				[this]() { return abortLoad.load(); });
			if (abortLoad) delete l;
			else delete pending.exchange(l);
			loading = false;
		});
	}
	void loadFolder(const std::string& dir) { loadFolders({dir}); }
	void addFolders(const std::vector<std::string>& dirs) {
		std::vector<std::string> all = folders;
		all.insert(all.end(), dirs.begin(), dirs.end());
		loadFolders(all);
	}
	void reload() { if (!folders.empty()) loadFolders(std::vector<std::string>(folders)); }
	std::string lastFolder() { return folders.empty() ? "" : folders.back(); }

	static void scan(const std::string& dir, std::vector<std::string>& out, int depth) {
		for (const std::string& p : system::getEntries(dir)) {
			std::string fn = system::getFilename(p);
			if (fn.empty() || fn[0] == '.') continue;
			if (system::isDirectory(p)) { if (depth > 0) scan(p, out, depth - 1); }
			else if (stella::isAudioName(fn)) out.push_back(p);
			if (out.size() > 4 * stella::MAX_FILES) return;
		}
	}

	bool fixedAxes() { return params[AXIS_PARAM].getValue() > 0.5f; }

	// where voice v's point is right now (knob + CV + drift/gravity), 0..1
	void voicePos(int v, float& x, float& y) {
		x = params[X_PARAM + v].getValue() + inputs[X_INPUT + v].getVoltage() * 0.1f + offX[v];
		y = params[Y_PARAM + v].getValue() + inputs[Y_INPUT + v].getVoltage() * 0.1f + offY[v];
		x = clamp(x, 0.f, 1.f);
		y = clamp(y, 0.f, 1.f);
	}

	void process(const ProcessArgs& args) override {
		// a newly loaded folder is ready: swap it in (only once the old garbage is cleared)
		if (pending.load() && !garbage.load()) {
			stella::Library* l = pending.exchange(nullptr);
			for (int v = 0; v < NV; v++) { cur[v].e = nullptr; old[v].e = nullptr; lastHit[v] = -1; }
			garbage = lib;
			lib = l;
		}
		bool fixed = fixedAxes();

		// slow stuff: drift, springs back to the knobs, screen info
		if (++ctrl >= 32) {
			ctrl = 0;
			float dt = 32.f * args.sampleTime;
			float drift = clamp(params[DRIFT_PARAM].getValue() + inputs[DRIFT_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
			float grav = params[GRAVITY_PARAM].getValue();
			bool active = drift > 0.001f || grav > 0.001f;
			float spring = dt / (active ? 25.f : 1.5f);
			float speed = drift * drift * 0.12f;  // map widths per second, at most
			for (int v = 0; v < NV; v++) {
				if (drift > 0.001f) {
					// velocity wanders smoothly, so the path curves rather than jitters
					float k = std::min(1.f, dt * 1.5f);
					driftVX[v] += (random::normal() - driftVX[v]) * k;
					driftVY[v] += (random::normal() - driftVY[v]) * k;
					offX[v] += driftVX[v] * speed * dt;
					offY[v] += driftVY[v] * speed * dt;
				}
				offX[v] -= offX[v] * spring;
				offY[v] -= offY[v] * spring;
				// don't let the offset push past the map edge for long
				float x, y;
				voicePos(v, x, y);
				float bx = params[X_PARAM + v].getValue() + inputs[X_INPUT + v].getVoltage() * 0.1f;
				float by = params[Y_PARAM + v].getValue() + inputs[Y_INPUT + v].getVoltage() * 0.1f;
				offX[v] = clamp(offX[v], -bx - 0.02f, 1.02f - bx);
				offY[v] = clamp(offY[v], -by - 0.02f, 1.02f - by);
				posX[v] = x;
				posY[v] = y;
				radius[v] = stella::radiusFor(params[SPREAD_PARAM + v].getValue());
				flash[v] = std::max(0.f, flash[v] - dt * 3.f);
			}
		}

		float mix = 0.f;
		for (int v = 0; v < NV; v++) {
			if (trig[v].process(inputs[TRIG_INPUT + v].getVoltage(), 0.1f, 1.f) && lib && !lib->entries.empty()) {
				float x, y;
				voicePos(v, x, y);
				float chance = clamp(params[CHANCE_PARAM + v].getValue() + inputs[CHANCE_INPUT + v].getVoltage() * 0.1f, 0.f, 1.f);
				float r = stella::radiusFor(params[SPREAD_PARAM + v].getValue());
				int i = stella::pick(*lib, fixed, x, y, r, chance, random::uniform(), random::uniform());
				if (i >= 0) {
					const stella::Entry& e = lib->entries[i];
					// let the previous sound die away over 5 ms instead of cutting it
					old[v] = cur[v];
					if (old[v].e) old[v].fade = old[v].gain / (0.005f * args.sampleRate);
					cur[v].e = &e;
					cur[v].pos = 0;
					cur[v].inc = (double) e.rate * args.sampleTime;
					cur[v].gain = 1.f;
					cur[v].fade = 0.f;
					lastHit[v] = i;
					flash[v] = 1.f;
					float grav = params[GRAVITY_PARAM].getValue();
					if (grav > 0.f) {
						offX[v] += (e.x(fixed) - x) * grav * 0.35f;
						offY[v] += (e.y(fixed) - y) * grav * 0.35f;
					}
				}
			}
			float s = (cur[v].next() + old[v].next()) * 5.f;
			if (outputs[OUT_OUTPUT + v].isConnected()) outputs[OUT_OUTPUT + v].setVoltage(s);
			else mix += s;
		}
		outputs[MIX_OUTPUT].setVoltage(mix);
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		for (int v = 0; v < NV; v++) {
			if (cur[v].e) cur[v].inc = (double) cur[v].e->rate * e.sampleTime;
			if (old[v].e) old[v].inc = (double) old[v].e->rate * e.sampleTime;
		}
	}

	json_t* dataToJson() override {
		json_t* r = json_object();
		json_t* fs = json_array();
		for (const std::string& f : folders) json_array_append_new(fs, json_string(f.c_str()));
		json_object_set_new(r, "folders", fs);
		json_object_set_new(r, "subfolders", json_boolean(subfolders));
		json_object_set_new(r, "normalise", json_boolean(normalise));
		return r;
	}

	void dataFromJson(json_t* r) override {
		if (json_t* j = json_object_get(r, "subfolders")) subfolders = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "normalise")) normalise = json_boolean_value(j);
		std::vector<std::string> fs;
		if (json_t* a = json_object_get(r, "folders")) {
			size_t i;
			json_t* v;
			json_array_foreach(a, i, v) if (json_string_value(v)) fs.push_back(json_string_value(v));
		}
		else if (json_t* j = json_object_get(r, "folder")) if (json_string_value(j)) fs.push_back(json_string_value(j));
		if (!fs.empty()) loadFolders(fs);
	}
};

// ---- the map screen ---------------------------------------------------------
struct MapDisplay : Widget {
	Stella* module = nullptr;
	float morph = -1.f;   // 0 = AUTO layout drawn, 1 = FIXED; eases between them
	Vec hover = Vec(-1, -1);
	int dragVoice = -1;
	Vec dragPos;
	std::string font = asset::system("res/fonts/ShareTechMono-Regular.ttf");

	static constexpr float HEAD = 5.f;  // mm of header strip at the top

	Rect mapRect() {
		float head = mm2px(HEAD);
		float pad = mm2px(1.5f);
		return Rect(Vec(pad, head + pad * 0.5f), Vec(box.size.x - 2 * pad, box.size.y - head - 1.5f * pad));
	}
	Vec toPx(float x, float y) {
		Rect r = mapRect();
		return Vec(r.pos.x + x * r.size.x, r.pos.y + (1.f - y) * r.size.y);
	}
	void toMap(Vec p, float& x, float& y) {
		Rect r = mapRect();
		x = clamp((p.x - r.pos.x) / r.size.x, 0.f, 1.f);
		y = clamp(1.f - (p.y - r.pos.y) / r.size.y, 0.f, 1.f);
	}

	float ex(const stella::Entry& e) { return e.ax + (e.fx - e.ax) * morph; }
	float ey(const stella::Entry& e) { return e.ay + (e.fy - e.ay) * morph; }

	int nearestDot(Vec p, float maxPx) {
		if (!module || !module->lib) return -1;
		int best = -1;
		float bd = maxPx * maxPx;
		const auto& es = module->lib->entries;
		for (int i = 0; i < (int) es.size(); i++) {
			Vec q = toPx(ex(es[i]), ey(es[i]));
			float d = (q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y);
			if (d < bd) { bd = d; best = i; }
		}
		return best;
	}

	void step() override {
		Widget::step();
		if (!module) return;
		float target = module->fixedAxes() ? 1.f : 0.f;
		if (morph < 0.f) morph = target;
		morph += (target - morph) * 0.15f;
		if (std::fabs(target - morph) < 0.002f) morph = target;
	}

	void text(NVGcontext* vg, std::shared_ptr<window::Font> f, float x, float y, float size, NVGcolor c, int align, const std::string& s) {
		nvgFontFaceId(vg, f->handle);
		nvgFontSize(vg, size);
		nvgFillColor(vg, c);
		nvgTextAlign(vg, align);
		nvgText(vg, x, y, s.c_str(), NULL);
	}

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, box.size.x, box.size.y, mm2px(1.2f));
		nvgFillColor(vg, nvgRGB(0x05, 0x07, 0x0f));
		nvgFill(vg);
		Widget::draw(args);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) return Widget::drawLayer(args, layer);
		NVGcontext* vg = args.vg;
		std::shared_ptr<window::Font> f = APP->window->loadFont(font);
		if (!f) return;
		nvgSave(vg);
		nvgScissor(vg, 0, 0, box.size.x, box.size.y);
		Rect r = mapRect();
		// grid
		nvgStrokeColor(vg, nvgRGB(0x16, 0x1d, 0x36));
		nvgStrokeWidth(vg, 0.8f);
		for (int g = 1; g < 4; g++) {
			nvgBeginPath(vg);
			nvgMoveTo(vg, r.pos.x + r.size.x * g / 4, r.pos.y);
			nvgLineTo(vg, r.pos.x + r.size.x * g / 4, r.pos.y + r.size.y);
			nvgMoveTo(vg, r.pos.x, r.pos.y + r.size.y * g / 4);
			nvgLineTo(vg, r.pos.x + r.size.x, r.pos.y + r.size.y * g / 4);
			nvgStroke(vg);
		}
		float hy = mm2px(3.6f);
		NVGcolor dim = nvgRGB(0x7f, 0x8a, 0xa8), soft = nvgRGB(0xcf, 0xd6, 0xea);
		bool fixed = !module || module->fixedAxes();

		stella::Library* lib = module ? module->lib : nullptr;
		if (!module) {
			// module browser preview: a few made-up stars
			random::Xoroshiro128Plus rng;
			rng.seed(7, 11);
			for (int i = 0; i < 120; i++) {
				float x = (rng() % 1000) / 1000.f, y = (rng() % 1000) / 1000.f;
				Vec p = toPx(x, y);
				nvgBeginPath(vg);
				nvgCircle(vg, p.x, p.y, 1.2f);
				nvgFillColor(vg, nvgRGBA(0xff, 0xff, 0xff, 0x90));
				nvgFill(vg);
			}
			text(vg, f, mm2px(1.5f), hy, 9.f, soft, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE, "STELLA");
			nvgRestore(vg);
			return;
		}

		// dots
		if (lib) {
			const auto& es = lib->entries;
			float dotR = es.size() > 400 ? 1.0f : es.size() > 150 ? 1.4f : 1.9f;
			for (const auto& e : es) {
				Vec p = toPx(ex(e), ey(e));
				// warm (heavy, low) .. white .. cool (bright, noisy)
				float c = e.colour;
				NVGcolor col = c < 0.5f ? nvgLerpRGBA(nvgRGB(0xff, 0xc0, 0x70), nvgRGB(0xf0, 0xec, 0xe4), c * 2.f)
				                        : nvgLerpRGBA(nvgRGB(0xf0, 0xec, 0xe4), nvgRGB(0x9a, 0xe4, 0xff), c * 2.f - 1.f);
				col.a = 0.8f;
				nvgBeginPath(vg);
				nvgCircle(vg, p.x, p.y, dotR);
				nvgFillColor(vg, col);
				nvgFill(vg);
			}
		}

		// voices: ring, crosshair, last-played sample
		for (int v = 0; v < NV; v++) {
			NVGcolor c = VOICE_COL[v];
			Vec p = toPx(module->posX[v], module->posY[v]);
			float rad = module->radius[v] * r.size.x;
			nvgBeginPath(vg);
			nvgCircle(vg, p.x, p.y, rad);
			nvgFillColor(vg, nvgTransRGBAf(c, 0.07f));
			nvgFill(vg);
			nvgStrokeColor(vg, nvgTransRGBAf(c, 0.75f));
			nvgStrokeWidth(vg, 0.9f);
			nvgStroke(vg);
			nvgBeginPath(vg);
			nvgMoveTo(vg, p.x - 6, p.y); nvgLineTo(vg, p.x + 6, p.y);
			nvgMoveTo(vg, p.x, p.y - 6); nvgLineTo(vg, p.x, p.y + 6);
			nvgStrokeColor(vg, c);
			nvgStrokeWidth(vg, 1.2f);
			nvgStroke(vg);
			text(vg, f, p.x + 7, p.y - 4, 8.f, c, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE, std::to_string(v + 1));
			int h = module->lastHit[v];
			if (lib && h >= 0 && h < (int) lib->entries.size()) {
				Vec q = toPx(ex(lib->entries[h]), ey(lib->entries[h]));
				float fl = module->flash[v];
				nvgBeginPath(vg);
				nvgCircle(vg, q.x, q.y, 3.2f + 3.f * fl);
				nvgStrokeColor(vg, nvgTransRGBAf(c, 0.5f + 0.5f * fl));
				nvgStrokeWidth(vg, 1.1f);
				nvgStroke(vg);
				if (fl > 0.f) {
					nvgBeginPath(vg);
					nvgCircle(vg, q.x, q.y, 2.2f);
					nvgFillColor(vg, nvgTransRGBAf(c, fl));
					nvgFill(vg);
				}
			}
		}

		// header strip
		nvgBeginPath(vg);
		nvgRect(vg, 0, 0, box.size.x, mm2px(HEAD));
		nvgFillColor(vg, nvgRGBA(0x05, 0x07, 0x0f, 0xe0));
		nvgFill(vg);
		std::string head;
		if (module->loading) {
			head = "analysing " + std::to_string(module->loadDone.load()) + "/" + std::to_string(module->loadTotal.load()) + "...";
		}
		else if (lib) {
			head = lib->folder + " - " + std::to_string(lib->entries.size()) + " samples";
			if (!lib->note.empty()) head += " - " + lib->note;
			if (lib->entries.empty()) head = lib->folder + " - no WAV/AIFF files found";
		}
		else head = "press LOAD or drop folders here";
		if (head.size() > 44) head = head.substr(0, 41) + "...";
		text(vg, f, mm2px(1.5f), hy, 9.f, soft, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE, head);
		text(vg, f, box.size.x - mm2px(1.5f), hy, 9.f, VOICE_COL[0], NVG_ALIGN_RIGHT | NVG_ALIGN_BASELINE, fixed ? "FIXED" : "AUTO");

		// axis hints
		if (fixed) {
			text(vg, f, box.size.x - mm2px(1.5f), box.size.y - mm2px(1.2f), 8.f, dim, NVG_ALIGN_RIGHT | NVG_ALIGN_BASELINE, "BRIGHT >");
			nvgSave(vg);
			nvgTranslate(vg, mm2px(2.8f), box.size.y - mm2px(1.5f));
			nvgRotate(vg, -M_PI / 2);
			text(vg, f, 0, 0, 8.f, dim, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE, "LONG >");
			nvgRestore(vg);
		}

		// name of the sample under the mouse (or being dragged onto)
		Vec probe = dragVoice >= 0 ? dragPos : hover;
		int d = probe.x >= 0 ? nearestDot(probe, dragVoice >= 0 ? 1e4f : 10.f) : -1;
		if (lib && d >= 0) {
			std::string nm = lib->entries[d].name;
			if (nm.size() > 30) nm = nm.substr(0, 27) + "...";
			text(vg, f, mm2px(1.5f) + (fixed ? mm2px(3.f) : 0), box.size.y - mm2px(1.2f), 8.5f, soft, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE, nm);
		}
		nvgRestore(vg);
		Widget::drawLayer(args, layer);
	}

	// click or drag on the map: the nearest voice's point follows the mouse
	void moveVoice(int v, Vec p) {
		float x, y;
		toMap(p, x, y);
		float bx = x - module->inputs[Stella::X_INPUT + v].getVoltage() * 0.1f - module->offX[v];
		float by = y - module->inputs[Stella::Y_INPUT + v].getVoltage() * 0.1f - module->offY[v];
		module->params[Stella::X_PARAM + v].setValue(clamp(bx, 0.f, 1.f));
		module->params[Stella::Y_PARAM + v].setValue(clamp(by, 0.f, 1.f));
	}

	void onButton(const ButtonEvent& e) override {
		if (!module || e.button != GLFW_MOUSE_BUTTON_LEFT || e.action != GLFW_PRESS) return Widget::onButton(e);
		int best = 0;
		float bd = 1e9f;
		for (int v = 0; v < NV; v++) {
			Vec q = toPx(module->posX[v], module->posY[v]);
			float d = (q.x - e.pos.x) * (q.x - e.pos.x) + (q.y - e.pos.y) * (q.y - e.pos.y);
			if (d < bd) { bd = d; best = v; }
		}
		dragVoice = best;
		dragPos = e.pos;
		moveVoice(best, e.pos);
		e.consume(this);
	}
	void onDragMove(const DragMoveEvent& e) override {
		if (!module || dragVoice < 0) return;
		dragPos = dragPos.plus(e.mouseDelta.div(getAbsoluteZoom()));
		moveVoice(dragVoice, dragPos);
	}
	void onDragEnd(const DragEndEvent& e) override { dragVoice = -1; }
	void onHover(const HoverEvent& e) override { hover = e.pos; Widget::onHover(e); }
	void onLeave(const LeaveEvent& e) override { hover = Vec(-1, -1); Widget::onLeave(e); }
};

// ---- panel parts ------------------------------------------------------------
struct StellaKnob : SvgKnob {
	StellaKnob() {
		minAngle = -0.83f * M_PI;
		maxAngle = 0.83f * M_PI;
		shadow->opacity = 0.f;
	}
};
template <int V>
struct VoiceKnob : StellaKnob {
	VoiceKnob() { setSvg(Svg::load(asset::plugin(pluginInstance, "res/Knob" + std::to_string(V) + ".svg"))); }
};

struct AxisSwitch : SvgSwitch {
	AxisSwitch() {
		shadow->opacity = 0.f;
		addFrame(Svg::load(asset::plugin(pluginInstance, "res/Axis_0.svg")));
		addFrame(Svg::load(asset::plugin(pluginInstance, "res/Axis_1.svg")));
	}
};

struct LoadButton : SvgSwitch {
	LoadButton() {
		momentary = true;
		shadow->opacity = 0.f;
		addFrame(Svg::load(asset::plugin(pluginInstance, "res/Load_0.svg")));
		addFrame(Svg::load(asset::plugin(pluginInstance, "res/Load_1.svg")));
	}
};

static std::string chooseFolder(const std::string& start) {
	char* p = osdialog_file(OSDIALOG_OPEN_DIR, start.empty() ? NULL : start.c_str(), NULL, NULL);
	if (!p) return "";
	std::string s = p;
	std::free(p);
	return s;
}

struct StellaWidget : ModuleWidget {
	bool loadWasDown = false;

	StellaWidget(Stella* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Stella.svg")));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// positions in mm; must match res/make_panel.py
		MapDisplay* d = createWidget<MapDisplay>(mm2px(Vec(5.f, 12.5f)));
		d->box.size = mm2px(Vec(78.f, 78.f));
		d->module = module;
		addChild(d);

		const float gy = 106.f;
		addParam(createParamCentered<AxisSwitch>(mm2px(Vec(10.f, gy)), module, Stella::AXIS_PARAM));
		addParam(createParamCentered<LoadButton>(mm2px(Vec(29.f, gy)), module, Stella::LOAD_PARAM));
		addParam(createParamCentered<VoiceKnob<0>>(mm2px(Vec(41.5f, gy)), module, Stella::DRIFT_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(52.5f, gy)), module, Stella::DRIFT_INPUT));
		addParam(createParamCentered<VoiceKnob<0>>(mm2px(Vec(65.5f, gy)), module, Stella::GRAVITY_PARAM));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(78.f, gy)), module, Stella::MIX_OUTPUT));

		const float cx[NV] = {111.f, 129.f, 147.f, 165.f};
		for (int v = 0; v < NV; v++) {
			float x = cx[v];
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x, 19.f)), module, Stella::TRIG_INPUT + v));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x, 42.5f)), module, Stella::X_INPUT + v));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x, 65.5f)), module, Stella::Y_INPUT + v));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x, 100.5f)), module, Stella::CHANCE_INPUT + v));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x, 112.5f)), module, Stella::OUT_OUTPUT + v));
			const float ky[4] = {31.f, 54.f, 77.f, 89.f};
			const int kp[4] = {Stella::X_PARAM, Stella::Y_PARAM, Stella::SPREAD_PARAM, Stella::CHANCE_PARAM};
			for (int k = 0; k < 4; k++) {
				Vec p = mm2px(Vec(x, ky[k]));
				int id = kp[k] + v;
				switch (v) {
					case 0: addParam(createParamCentered<VoiceKnob<1>>(p, module, id)); break;
					case 1: addParam(createParamCentered<VoiceKnob<2>>(p, module, id)); break;
					case 2: addParam(createParamCentered<VoiceKnob<3>>(p, module, id)); break;
					default: addParam(createParamCentered<VoiceKnob<4>>(p, module, id)); break;
				}
			}
		}
	}

	void step() override {
		ModuleWidget::step();
		Stella* m = dynamic_cast<Stella*>(module);
		if (!m) return;
		delete m->garbage.exchange(nullptr);
		// LOAD is read here on the UI thread, so the folder dialog never blocks audio
		bool down = m->params[Stella::LOAD_PARAM].getValue() > 0.5f;
		if (down && !loadWasDown) {
			std::string dir = chooseFolder(m->lastFolder());
			if (!dir.empty()) m->loadFolder(dir);
		}
		loadWasDown = down;
	}

	void onPathDrop(const PathDropEvent& e) override {
		Stella* m = dynamic_cast<Stella*>(module);
		if (!m || e.paths.empty()) return;
		// every folder dropped goes on the map; a dropped file brings its folder.
		// Hold shift to add them to the folders already loaded.
		std::vector<std::string> dirs;
		for (std::string p : e.paths) {
			if (!system::isDirectory(p)) p = system::getDirectory(p);
			dirs.push_back(p);
		}
		if ((APP->window->getMods() & RACK_MOD_MASK) == GLFW_MOD_SHIFT) m->addFolders(dirs);
		else m->loadFolders(dirs);
		e.consume(this);
	}

	void appendContextMenu(Menu* menu) override {
		Stella* m = dynamic_cast<Stella*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		if (m->folders.empty()) menu->addChild(createMenuLabel("No folder loaded"));
		for (const std::string& f : m->folders) menu->addChild(createMenuLabel("Folder: " + f));
		menu->addChild(createMenuItem("Load folder (replace)...", "", [=]() {
			std::string dir = chooseFolder(m->lastFolder());
			if (!dir.empty()) m->loadFolder(dir);
		}));
		menu->addChild(createMenuItem("Add folder...", "", [=]() {
			std::string dir = chooseFolder(m->lastFolder());
			if (!dir.empty()) m->addFolders({dir});
		}));
		if (m->folders.size() > 1) {
			menu->addChild(createSubmenuItem("Remove folder", "", [=](Menu* sub) {
				for (size_t i = 0; i < m->folders.size(); i++) {
					std::string f = m->folders[i];
					sub->addChild(createMenuItem(system::getFilename(f), "", [=]() {
						std::vector<std::string> keep;
						for (const std::string& g : m->folders) if (g != f) keep.push_back(g);
						m->loadFolders(keep);
					}));
				}
			}));
		}
		if (!m->folders.empty())
			menu->addChild(createMenuItem("Re-scan folders", "", [=]() { m->reload(); }));
		menu->addChild(createBoolMenuItem("Include subfolders", "",
			[=]() { return m->subfolders; },
			[=](bool b) { m->subfolders = b; m->reload(); }));
		menu->addChild(createBoolMenuItem("Even out sample levels", "",
			[=]() { return m->normalise; },
			[=](bool b) { m->normalise = b; m->reload(); }));
	}
};

Model* modelStella = createModel<Stella, StellaWidget>("Stella");
