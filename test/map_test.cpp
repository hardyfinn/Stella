// Loads a folder, prints each sample's measurements and map positions as CSV,
// and checks the picker.  make test DIR=/path/to/samples
#include "../src/Analysis.hpp"
#include <dirent.h>
#include <sys/stat.h>
#include <cassert>
#include <random>
using namespace stella;

static void scan(const std::string& d, std::vector<std::string>& out, int depth) {
	DIR* dir = opendir(d.c_str());
	if (!dir) return;
	while (dirent* de = readdir(dir)) {
		std::string fn = de->d_name;
		if (fn.empty() || fn[0] == '.') continue;
		std::string p = d + "/" + fn;
		struct stat st;
		if (stat(p.c_str(), &st)) continue;
		if (S_ISDIR(st.st_mode)) { if (depth < 4) scan(p, out, depth + 1); }
		else if (isAudioName(fn)) out.push_back(p);
	}
	closedir(dir);
}

int main(int argc, char** argv) {
	std::vector<std::string> files;
	for (int i = 1; i < argc; i++) scan(argv[i], files, 0);
	std::sort(files.begin(), files.end());
	Library* lib = loadFiles(argc > 1 ? argv[1] : "", files, true, nullptr, nullptr);
	std::printf("name,bright,noise,attack,length,weight,ax,ay,fx,fy,colour\n");
	for (auto& e : lib->entries)
		std::printf("%s,%.2f,%.2f,%.2f,%.2f,%.2f,%.3f,%.3f,%.3f,%.3f,%.2f\n", e.name.c_str(), e.feat[0], e.feat[1], e.feat[2],
		            e.feat[3], e.feat[4], e.ax, e.ay, e.fx, e.fy, e.colour);
	std::fprintf(stderr, "%zu samples, note: %s\n", lib->entries.size(), lib->note.c_str());
	// picker: chance 0 always nearest; chance 1 never nearest when neighbours exist
	if (lib->entries.size() > 3) {
		std::mt19937 rng(1);
		std::uniform_real_distribution<float> u(0, 1);
		int n0 = nearest(*lib, false, 0.5f, 0.5f);
		for (int i = 0; i < 200; i++) assert(pick(*lib, false, 0.5f, 0.5f, 0.3f, 0.f, u(rng), u(rng)) == n0);
		int same = 0;
		for (int i = 0; i < 200; i++) same += pick(*lib, false, 0.5f, 0.5f, 0.3f, 1.f, u(rng), u(rng)) == n0;
		assert(same == 0);
		std::fprintf(stderr, "picker OK\n");
	}
	delete lib;
}
