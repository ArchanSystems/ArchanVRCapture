// Replays a recorded head-motion file (poses-*.csv from the plugin's recorder) exactly the way the
// plugin runs: OBS renders every 1/fps s (times from the file's obs_time column), new frames arrive
// when OBS saw them, the game clock is mapped onto the OBS clock with the same offset filter, the
// output time is mapped(now) - delay, the frame nearest to it is shown through the camera planned
// for that time. Writes per OBS frame: tick time, output time, the shown frame's head orientation,
// the camera, and (for comparison) the head orientation the plain, unstabilized picture shows.
//
// usage: stab_replay <poses.csv> <out.csv> <crop %> <delay s> <smooth 0..100> <horizon 0|1> [fps]
#include "../obs-plugin/stabilizer.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace stab;

struct Row {
	double obsT, t;
	Q q;
	double tan[4];
	V3 up;
	int upKnown;
	unsigned w, h;
};

int main(int argc, char **argv)
{
	if (argc < 7) {
		printf("usage: stab_replay <poses.csv> <out.csv> <crop %%> <delay s> <smooth 0..100> <horizon 0|1> [fps]\n");
		return 2;
	}
	FILE *f = fopen(argv[1], "r");
	if (!f) {
		printf("cannot open %s\n", argv[1]);
		return 2;
	}
	char line[1024];
	fgets(line, sizeof(line), f); // header
	std::vector<Row> rows;
	while (fgets(line, sizeof(line), f)) {
		Row r;
		unsigned long long obsT;
		long long poseT;
		char src[16];
		if (sscanf_s(line, "%llu,%lld,%15[^,],%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%d,%u,%u", &obsT, &poseT, src,
			     (unsigned)sizeof(src), &r.q.x, &r.q.y, &r.q.z, &r.q.w, &r.tan[0], &r.tan[1], &r.tan[2], &r.tan[3],
			     &r.up.x, &r.up.y, &r.up.z, &r.upKnown, &r.w, &r.h) != 17)
			continue;
		r.obsT = obsT * 1e-9;
		r.t = poseT * 1e-9;
		rows.push_back(r);
	}
	fclose(f);
	if (rows.size() < 10) {
		printf("too few rows\n");
		return 2;
	}

	Settings st;
	st.crop = atof(argv[3]);
	st.delay = atof(argv[4]);
	st.smooth = atof(argv[5]);
	st.horizon = atoi(argv[6]) != 0;
	const double fps = argc > 7 ? atof(argv[7]) : 30.0;
	LookAhead la;
	la.Configure(st);
	la.SetFrameRate(fps);
	if (const char *it = getenv("STAB_ITERS")) // experiments only
		la.kIterations = atoi(it);
	if (const char *v = getenv("STAB_HW"))
		la.kHorizonW = atof(v);
	// robustness: STAB_SKIP = share of OBS frames not rendered (lagged frames), deterministic
	const double skip = getenv("STAB_SKIP") ? atof(getenv("STAB_SKIP")) : 0.0;
	unsigned rng = 12345;

	FILE *o = fopen(argv[2], "w");
	fprintf(o, "tick,T,frame_t,hqx,hqy,hqz,hqw,cqx,cqy,cqz,cqw,pqx,pqy,pqz,pqw,pulled,fits,guard_deg,level_fits\n");
	size_t next = 0, frames = 0, edges = 0, pulledN = 0;
	std::vector<size_t> buffered;
	// STAB_KEEP_MS = margin (ms): also run the plugin's frame retention rule (FramesToKeep) and check
	// that the frame picked from the kept frames is the same as from all frames, every OBS frame
	const double keepMargin = getenv("STAB_KEEP_MS") ? atof(getenv("STAB_KEEP_MS")) * 1e-3 : -1.0;
	const double keepGrow = getenv("STAB_KEEP_GROW") ? atof(getenv("STAB_KEEP_GROW")) : 0.0; // margin per s ahead
	std::vector<size_t> kept;
	std::vector<double> keptTimes;
	std::vector<char> keepFlags;
	size_t picksCompared = 0, picksDiffer = 0, keptMax = 0, oldMax = 0;
	double keptSum = 0, oldSum = 0, prevT = 0;
	bool haveOffset = false;
	double offset = 0, worstMs = 0, sumMs = 0;
	for (double tick = rows[0].obsT; tick <= rows.back().obsT; tick += 1.0 / fps) {
		while (next < rows.size() && rows[next].obsT <= tick + 1e-9) { // frames OBS has seen by now
			const Row &r = rows[next];
			la.AddPose(r.t, r.q, r.up, r.upKnown != 0);
			const double off = r.t - r.obsT;
			if (!haveOffset || std::fabs(off - offset) > 1.0) {
				offset = off;
				haveOffset = true;
			} else {
				offset += (off - offset) * 0.02;
			}
			buffered.push_back(next);
			kept.push_back(next);
			next++;
		}
		if (buffered.empty())
			continue;
		rng = rng * 1103515245u + 12345u;
		if (skip > 0 && ((rng >> 8) & 0xFFFF) / 65536.0 < skip)
			continue; // this OBS frame lagged: not rendered
		const double T = tick + offset - st.delay;
		const double lastT = prevT;
		prevT = T;
		if (keepMargin >= 0) {
			keptTimes.clear();
			for (size_t b : kept)
				keptTimes.push_back(rows[b].t);
			FramesToKeep(keptTimes, T, 1.0 / fps, keepMargin, keepGrow, keepFlags);
			size_t w = 0;
			for (size_t i = 0; i < kept.size(); i++)
				if (keepFlags[i])
					kept[w++] = kept[i];
			kept.resize(w);
		}
		if (!la.Ready(T))
			continue; // the plugin shows the plain picture until the delay buffer is filled
		// frame nearest to T (stale frames far away are never nearest once real ones exist)
		size_t pick = buffered[0];
		for (size_t b : buffered)
			if (std::fabs(rows[b].t - T) < std::fabs(rows[pick].t - T))
				pick = b;
		if (std::fabs(rows[pick].t - T) > 0.5)
			continue; // buffer not filled yet
		if (keepMargin >= 0) {
			size_t pk = kept[0];
			for (size_t b : kept)
				if (std::fabs(rows[b].t - T) < std::fabs(rows[pk].t - T))
					pk = b;
			picksCompared++;
			const bool differ = rows[pk].t != rows[pick].t; // same time = same frame time (a tie)
			picksDiffer += differ;
			if (differ && picksDiffer <= 5)
				printf("  differs at tick %.3f s: T %.4f, all frames -> %.4f (|d| %.1f ms), kept -> %.4f (|d| %.1f ms), %zu kept, "
				       "previous T %.4f\n",
				       tick - rows[0].obsT, T, rows[pick].t, std::fabs(rows[pick].t - T) * 1e3, rows[pk].t,
				       std::fabs(rows[pk].t - T) * 1e3, kept.size(), lastT);
			size_t old = 0; // what the plugin keeps today: from T - 0.5 s to the newest frame
			for (size_t b : buffered)
				old += rows[b].t >= T - 0.5;
			keptSum += (double)kept.size();
			oldSum += (double)old;
			keptMax = std::max(keptMax, kept.size());
			oldMax = std::max(oldMax, old);
		}
		const Row &r = rows[pick];
		Image img;
		const uint32_t cw = r.w & ~31u, ch = cw / 16u * 9u;
		img.W = r.w;
		img.H = r.h;
		img.cropW = cw;
		img.cropH = ch;
		img.cropX = (r.w - cw) / 2u;
		img.cropY = (r.h - ch) / 2u;
		img.tl = r.tan[0];
		img.tr = r.tan[1];
		img.tu = r.tan[2];
		img.td = r.tan[3];
		bool pulled = false;
		const Q head = NormalizeQ(r.q);
		const auto c0 = std::chrono::steady_clock::now();
		const Q cam = la.Camera(img, T, head, &pulled);
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
		worstMs = std::max(worstMs, ms);
		sumMs += ms;
		const bool fits = Fits(img, head, cam, la.ZoomK());
		bool lvOk = false;
		const Q lv = RemoveRoll(cam, r.up, &lvOk);
		const int levelFits = (lvOk && r.upKnown && Fits(img, head, lv, la.ZoomK())) ? 1 : 0; // a level camera would fit
		edges += !fits;
		pulledN += pulled;
		frames++;
		const Q plain = NormalizeQ(rows[buffered.back()].q); // what the unstabilized picture shows now
		fprintf(o, "%.6f,%.6f,%.6f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%d,%d,%.4f,%d\n", tick, T, r.t,
			head.x, head.y, head.z, head.w, cam.x, cam.y, cam.z, cam.w, plain.x, plain.y, plain.z, plain.w,
			pulled ? 1 : 0, fits ? 1 : 0, la.lastGuard, levelFits);
	}
	fclose(o);
	printf("crop %.1f %% delay %.2f s smooth %.0f horizon %d: %zu OBS frames, room touched %.1f %%, edges %zu, "
	       "camera time per frame avg %.2f ms max %.2f ms\n",
	       st.crop, st.delay, st.smooth, st.horizon ? 1 : 0, frames, frames ? 100.0 * pulledN / frames : 0.0, edges,
	       frames ? sumMs / frames : 0.0, worstMs);
	if (keepMargin >= 0)
		printf("frame retention (margin %.1f ms): kept frames avg %.1f max %zu (today avg %.1f max %zu); picked frame "
		       "differs in %zu of %zu OBS frames\n",
		       keepMargin * 1e3, picksCompared ? keptSum / picksCompared : 0.0, keptMax,
		       picksCompared ? oldSum / picksCompared : 0.0, oldMax, picksDiffer, picksCompared);
	return edges || picksDiffer ? 1 : 0;
}
