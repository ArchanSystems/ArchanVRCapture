// Archan VR Capture - head-pose video stabilization (pure math, no OBS / D3D).
//
// Input per frame: the orientation the game rendered the left eye with, the image FOV
// (tangents) and its time. The video is shown a user-set delay late (look-ahead), so the
// camera for each frame is a centred, Gaussian-weighted mean of the head orientation around
// that frame (no lag on turns), optionally with the head tilt removed. The output picture is
// the eye image re-projected to that camera: a homography H that maps output coordinates
// (u,v) in [0,1]^2 to texture coordinates of the eye image. The crop is set by the user
// (no automatic measuring); beyond 60 % of the room it gives, the camera's difference to the
// real view is compressed smoothly, so the picture never shows an edge.
//
// Conventions (OpenXR and OpenVR): x right, y up, the view looks along -z.
// q rotates view coordinates into the tracking space.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace stab {

struct V3 {
	double x, y, z;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline double Len(V3 a) { return std::sqrt(Dot(a, a)); }
inline V3 Normalize(V3 a)
{
	const double l = Len(a);
	return l > 0 ? a * (1.0 / l) : a;
}

struct Q {
	double x, y, z, w;
};
inline Q Mul(Q a, Q b)
{
	return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Q Conj(Q q) { return {-q.x, -q.y, -q.z, q.w}; }
inline Q NormalizeQ(Q q)
{
	const double l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
	return l > 0 ? Q{q.x / l, q.y / l, q.z / l, q.w / l} : Q{0, 0, 0, 1};
}
inline V3 Rotate(Q q, V3 v)
{
	const V3 u = {q.x, q.y, q.z};
	const V3 t = Cross(u, v) * 2.0;
	return v + t * q.w + Cross(u, t);
}
// rotation vector (axis * angle) -> quaternion
inline Q Exp(V3 r)
{
	const double a = Len(r);
	if (a < 1e-12)
		return NormalizeQ({r.x * 0.5, r.y * 0.5, r.z * 0.5, 1.0});
	const double s = std::sin(a * 0.5) / a;
	return {r.x * s, r.y * s, r.z * s, std::cos(a * 0.5)};
}
// quaternion -> rotation vector (shortest way)
inline V3 Log(Q q)
{
	if (q.w < 0)
		q = {-q.x, -q.y, -q.z, -q.w};
	const V3 v = {q.x, q.y, q.z};
	const double vl = Len(v);
	if (vl < 1e-12)
		return v * 2.0;
	return v * (2.0 * std::atan2(vl, q.w) / vl);
}
inline double Angle(Q a, Q b) { return Len(Log(Mul(Conj(a), b))); }
inline Q Slerp(Q a, Q b, double t) { return NormalizeQ(Mul(a, Exp(Log(Mul(Conj(a), b)) * t))); }

// rotation matrix (columns = rotated x, y, z axes) -> quaternion
inline Q FromColumns(V3 cx, V3 cy, V3 cz)
{
	const double m00 = cx.x, m10 = cx.y, m20 = cx.z;
	const double m01 = cy.x, m11 = cy.y, m21 = cy.z;
	const double m02 = cz.x, m12 = cz.y, m22 = cz.z;
	const double tr = m00 + m11 + m22;
	Q q;
	if (tr > 0) {
		const double s = std::sqrt(tr + 1.0) * 2.0;
		q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25 * s};
	} else if (m00 > m11 && m00 > m22) {
		const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
		q = {0.25 * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
	} else if (m11 > m22) {
		const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
		q = {(m01 + m10) / s, 0.25 * s, (m12 + m21) / s, (m02 - m20) / s};
	} else {
		const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
		q = {(m02 + m20) / s, (m12 + m21) / s, 0.25 * s, (m10 - m01) / s};
	}
	return NormalizeQ(q);
}

// Same view direction, but no roll relative to 'up'. ok=false when looking straight up/down.
inline Q RemoveRoll(Q q, V3 up, bool *ok)
{
	const V3 f = Rotate(q, {0, 0, -1});
	up = Normalize(up);
	if (std::fabs(Dot(f, up)) > 0.995) {
		*ok = false;
		return q;
	}
	const V3 right = Normalize(Cross(f, up));
	const V3 u2 = Cross(right, f);
	*ok = true;
	return FromColumns(right, u2, f * -1.0);
}

struct Mat3 {
	double m[3][3];
};
inline Mat3 MulM(const Mat3 &a, const Mat3 &b)
{
	Mat3 r = {};
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			for (int k = 0; k < 3; k++)
				r.m[i][j] += a.m[i][k] * b.m[k][j];
	return r;
}
inline Mat3 RotM(Q q) // columns = rotated axes
{
	const V3 cx = Rotate(q, {1, 0, 0}), cy = Rotate(q, {0, 1, 0}), cz = Rotate(q, {0, 0, 1});
	return {{{cx.x, cy.x, cz.x}, {cx.y, cy.y, cz.y}, {cx.z, cy.z, cz.z}}};
}
inline Mat3 Transpose(const Mat3 &a)
{
	Mat3 r;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			r.m[i][j] = a.m[j][i];
	return r;
}

// The eye image: size, its centre 16:9 crop (pixels) and FOV tangents.
struct Image {
	uint32_t W = 0, H = 0;
	uint32_t cropX = 0, cropY = 0, cropW = 0, cropH = 0;
	double tl = -1, tr = 1, tu = 1, td = -1; // tan(left) <0, tan(right), tan(up), tan(down) <0
	bool Same(const Image &o) const
	{
		return W == o.W && H == o.H && cropW == o.cropW && cropH == o.cropH && std::fabs(tl - o.tl) < 1e-4 &&
		       std::fabs(tr - o.tr) < 1e-4 && std::fabs(tu - o.tu) < 1e-4 && std::fabs(td - o.td) < 1e-4;
	}
};

// Output (u,v,1) -> texture (U*w, V*w, w). Output = view of camera qv, zoom k (1 = the plain
// centre 16:9 crop, <1 = cropped in). With qv == qa and k == 1 this is exactly the plain crop.
inline Mat3 Homography(const Image &img, Q qa, Q qv, double k)
{
	const double Sx = img.tr - img.tl, Sy = img.tu - img.td; // tangent spans (y measured downward)
	const double cx = img.tl + (img.cropX + img.cropW * 0.5) / img.W * Sx;
	const double cy = -img.tu + (img.cropY + img.cropH * 0.5) / img.H * Sy;
	const double sx = (double)img.cropW / img.W * Sx * k, sy = (double)img.cropH / img.H * Sy * k;
	const Mat3 A = {{{sx, 0, cx - 0.5 * sx}, {0, sy, cy - 0.5 * sy}, {0, 0, 1}}}; // (u,v,1) -> (X, Ydown, 1)
	const Mat3 B = {{{1, 0, 0}, {0, -1, 0}, {0, 0, -1}}};                         // -> ray in qv view
	const Mat3 M = MulM(Transpose(RotM(qa)), RotM(qv));                            // qv view -> qa view
	const Mat3 C = {{{1, 0, 0}, {0, -1, 0}, {0, 0, -1}}};                         // ray -> (X, Ydown, 1)*w
	const Mat3 D = {{{1.0 / Sx, 0, -img.tl / Sx}, {0, 1.0 / Sy, img.tu / Sy}, {0, 0, 1}}}; // -> (U, V, 1)
	return MulM(D, MulM(C, MulM(M, MulM(B, A))));
}

inline bool Apply(const Mat3 &h, double u, double v, double *U, double *V)
{
	const double x = h.m[0][0] * u + h.m[0][1] * v + h.m[0][2];
	const double y = h.m[1][0] * u + h.m[1][1] * v + h.m[1][2];
	const double w = h.m[2][0] * u + h.m[2][1] * v + h.m[2][2];
	if (w <= 1e-9)
		return false;
	*U = x / w;
	*V = y / w;
	return true;
}

// All four output corners land inside the eye image (lines map to lines, so the whole picture does).
inline bool Fits(const Image &img, Q qa, Q qv, double k)
{
	const Mat3 h = Homography(img, qa, qv, k);
	const double eps = 1e-6;
	for (int i = 0; i < 4; i++) {
		double U, V;
		if (!Apply(h, (i & 1) ? 1.0 : 0.0, (i & 2) ? 1.0 : 0.0, &U, &V))
			return false;
		if (U < -eps || U > 1 + eps || V < -eps || V > 1 + eps)
			return false;
	}
	return true;
}

// Largest zoom k in [kMin, 1] for which qv fits. Returns kMin if even kMin does not fit.
inline double NeededK(const Image &img, Q qa, Q qv, double kMin)
{
	if (Fits(img, qa, qv, 1.0))
		return 1.0;
	if (!Fits(img, qa, qv, kMin))
		return kMin;
	double lo = kMin, hi = 1.0; // lo fits, hi does not
	for (int i = 0; i < 30; i++) {
		const double mid = 0.5 * (lo + hi);
		(Fits(img, qa, qv, mid) ? lo : hi) = mid;
	}
	return lo;
}

// User settings - all set by the user, nothing automatic (user decision 2026-11-05).
struct Settings {
	bool enable = true;
	double smooth = 50;   // 0..100: how much of the look-ahead window is used for smoothing
	bool horizon = false; // keep horizon level (no head tilt)
	double crop = 15;     // percent of the picture cropped away (zoom in) = room the camera may move in
	double delay = 1.0;   // seconds the video runs behind the game (look-ahead window)
	bool operator==(const Settings &o) const
	{
		return enable == o.enable && smooth == o.smooth && horizon == o.horizon && crop == o.crop &&
		       delay == o.delay;
	}
	bool operator!=(const Settings &o) const { return !(*this == o); }
};

// Pull of the camera toward the head in the path optimisation, from the smoothness setting:
// smoothness 0 -> 1 (follows the head), 100 -> 0.0001 (smoothest path the crop allows).
inline double SmoothMu(const Settings &s)
{
	const double sm = std::min(100.0, std::max(0.0, s.smooth)) / 100.0;
	return std::pow(10.0, -6.0 * sm);
}

struct PoseSample {
	double t; // seconds (game frame display time)
	Q q;
};

// Look-ahead stabilizer.
// The video is shown 'delay' seconds late, so at the output time T every head pose up to T + delay
// is known. At each output the camera path on an even 30 Hz grid from T - 2h to T + delay is planned
// as the smoothest path (least angular acceleration) that keeps every frame inside the room the crop
// gives: minimise sum|x[k+1] - 2x[k] + x[k-1]|^2 + mu sum|x[k] - head[k]|^2 (+ horizon pull) subject
// to "camera k fits in head k's eye image" (FISTA with exact projection; warm-started from the
// previous plan). The points T - 2h and T - h are fixed to what the viewer was shown (the shown
// cameras; where none was shown at that time, e.g. a skipped OBS frame or the start: the previous
// plan's path, never an extrapolation), so the camera continues smoothly and a new plan never jumps.
// The camera for T is the plan at T. Evaluating it at the even OBS output time (not at the jittery
// capture time of the frame) keeps static scenery smooth.
// Measured 2026-11-05: re-planning the whole window from scratch each frame made the camera hop
// between plans on recorded sessions (more iterations made it worse); extrapolating the shown
// cameras for the fixed points drifted when render times were uneven (real-time plugin test); a
// path committed on a fixed grid ignored the shown-frame guard's corrections and was rougher.
class LookAhead {
public:
	int kIterations = 150;  // FISTA iterations per output frame (warm started)
	double kRoomUse = 0.92; // the plan keeps 8 % of the room spare
	// Keep horizon level: pull of the plan toward the level view. Planned like the rest of the path
	// (not forced), so when the room cannot hold the horizon the tilt eases in and out smoothly.
	// 0.03 chosen on the recorded Pimax/DCS sessions (2026-11-05 replay sweep 0.001..1).
	double kHorizonW = 0.03;
	double lastGuard = 0; // degrees the shown-frame guard moved the planned camera (diagnostics)
	// Plan grid = the OBS output frame rate (every shown frame is a grid point; set by the plugin).
	void SetFrameRate(double fps)
	{
		fps = std::min(240.0, std::max(10.0, fps));
		if (std::fabs(fps - fps_) > 1e-6)
			Restart();
		fps_ = fps;
	}

	void Configure(const Settings &s)
	{
		if (s.crop != st_.crop || s.delay != st_.delay || s.smooth != st_.smooth || s.horizon != st_.horizon)
			Restart();
		st_ = s;
	}
	const Settings &Config() const { return st_; }

	// Every new frame's pose, in time order. Out-of-order or repeated times are ignored; a jump of
	// more than 5 s (e.g. a stale first frame) starts over.
	void AddPose(double t, Q q, V3 up, bool upKnown)
	{
		if (!s_.empty()) {
			if (t <= s_.back().t)
				return;
			if (t - s_.back().t > 5.0) {
				s_.clear();
				Restart();
			}
		}
		s_.push_back({t, NormalizeQ(q)});
		up_ = up;
		upKnown_ = upKnown;
		const double keep = 2.0 * std::max(st_.delay, 0.05) + 1.0;
		while (s_.size() > 2 && s_.front().t < t - keep)
			s_.erase(s_.begin());
	}
	double Latest() const { return s_.empty() ? 0.0 : s_.back().t; }
	// The output time lies inside the poses received in this session (buffer filled; never a
	// stale frame from an earlier session).
	bool Ready(double T) const { return s_.size() >= 2 && T >= s_.front().t && T <= s_.back().t; }
	double ZoomK() const { return 1.0 - std::min(60.0, std::max(0.0, st_.crop)) / 100.0; }

	// Head orientation at time t (slerp between the known poses; held at the ends).
	Q HeadAt(double t) const
	{
		if (s_.empty())
			return {0, 0, 0, 1};
		if (t <= s_.front().t)
			return s_.front().q;
		if (t >= s_.back().t)
			return s_.back().q;
		size_t hi = 1;
		while (hi < s_.size() && s_[hi].t < t)
			hi++;
		const PoseSample &a = s_[hi - 1], &b = s_[hi];
		return Slerp(a.q, b.q, (t - a.t) / (b.t - a.t));
	}

	// Camera for output time T. qf = head pose of the frame that will be shown (re-projected to the
	// camera); the result always fits that frame. 'pulled' = the plan touches the room at T.
	Q Camera(const Image &img, double T, Q qf, bool *pulled = nullptr)
	{
		qf = NormalizeQ(qf);
		if (pulled)
			*pulled = false;
		const double k = ZoomK();
		const double kPlan = 1.0 - (1.0 - k) * kRoomUse;
		const double h = 1.0 / fps_;
		// the weights below were set for a 30 Hz grid: per second of path, the acceleration term scales
		// with h^3 and the pulls with 1/h, so they are scaled by (h * 30)^4 to mean the same at any rate
		const double rate = std::pow(h * 30.0, 4.0);
		const int K = std::max(1, (int)std::lround(std::max(st_.delay, 0.0) / h));
		if (haveLast_ && std::fabs(T - lastT_) < 1e-4)
			return lastCam_; // the same OBS frame rendered again
		if (haveLast_ && T < lastT_)
			Restart(); // output time went back
		Q fixA, fixB;
		const bool fixed = FixedAt(T - 2.0 * h, h, &fixA) && FixedAt(T - h, h, &fixB);
		const int c = fixed ? 2 : K; // index of T; window T - c*h .. T + K*h
		const int n = c + 1 + K;
		const int f0 = fixed ? 2 : 0; // first free point
		const Q ref = HeadAt(T);
		const bool level = st_.horizon && upKnown_;
		heads_.resize(n);
		a_.resize(n);
		x_.resize(n);
		y_.resize(n);
		xn_.resize(n);
		g_.resize(n);
		for (int j = 0; j < n; j++) {
			const double tj = T + (j - c) * h;
			heads_[j] = HeadAt(tj);
			a_[j] = Log(Mul(Conj(ref), heads_[j]));
			if (level) { // "keep horizon level": the camera follows the head's view direction, not its tilt
				bool ok = true;
				const Q hl = RemoveRoll(heads_[j], up_, &ok);
				if (ok)
					a_[j] = Log(Mul(Conj(ref), hl));
			}
			if (j < f0) {
				x_[j] = Log(Mul(Conj(ref), j == 0 ? fixA : fixB)); // what the viewer saw: fixed
				continue;
			}
			x_[j] = a_[j];
			Q warm;
			if (PlanAt(tj, &warm))
				x_[j] = Log(Mul(Conj(ref), warm));
		}
		const double mu = SmoothMu(st_) * rate;
		const double hw = kHorizonW * rate;
		const double L = 32.0 + 2.0 * mu + (level ? 2.0 * hw : 0.0);
		for (int j = f0; j < n; j++)
			x_[j] = Log(Mul(Conj(ref), Project(img, ref, heads_[j], x_[j], kPlan, nullptr)));
		y_ = x_;
		double tk = 1.0;
		for (int it = 0; it < kIterations; it++) {
			for (int j = 0; j < n; j++) {
				g_[j] = (y_[j] - a_[j]) * (2.0 * mu);
				if (level) { // pull toward the same view with the horizon level (planned, not forced)
					bool ok = true;
					const Q cq = NormalizeQ(Mul(ref, Exp(y_[j])));
					const Q lv = RemoveRoll(cq, up_, &ok);
					if (ok)
						g_[j] = g_[j] + (y_[j] - Log(Mul(Conj(ref), lv))) * (2.0 * hw);
				}
			}
			for (int j = 1; j + 1 < n; j++) {
				const V3 d2 = y_[j + 1] - y_[j] * 2.0 + y_[j - 1];
				g_[j - 1] = g_[j - 1] + d2 * 2.0;
				g_[j] = g_[j] - d2 * 4.0;
				g_[j + 1] = g_[j + 1] + d2 * 2.0;
			}
			for (int j = 0; j < f0; j++)
				xn_[j] = x_[j];
			for (int j = f0; j < n; j++)
				xn_[j] = Log(Mul(Conj(ref), Project(img, ref, heads_[j], y_[j] - g_[j] * (1.0 / L), kPlan,
								   (pulled && j == c && it == kIterations - 1) ? pulled : nullptr)));
			const double tn = 0.5 * (1.0 + std::sqrt(1.0 + 4.0 * tk * tk));
			for (int j = 0; j < n; j++)
				y_[j] = xn_[j] + (xn_[j] - x_[j]) * ((tk - 1.0) / tn);
			x_.swap(xn_);
			tk = tn;
		}
		plan_.resize(n);
		for (int j = 0; j < n; j++)
			plan_[j] = {T + (j - c) * h, NormalizeQ(Mul(ref, Exp(x_[j])))};
		Q cam = plan_[c].q;
		const Q planned = cam;
		// the shown frame was captured at a slightly different time than T: it must still fit
		if (!Fits(img, qf, cam, k))
			cam = Project(img, qf, qf, Log(Mul(Conj(qf), cam)), k, nullptr);
		if (!Fits(img, qf, cam, k)) { // numerics guard
			double lo = 0.0, hi = 1.0;
			for (int i = 0; i < 20; i++) {
				const double mid = 0.5 * (lo + hi);
				(Fits(img, qf, Slerp(cam, qf, mid), k) ? hi : lo) = mid;
			}
			cam = Slerp(cam, qf, hi);
		}
		lastGuard = 2.0 *
			    std::acos(std::min(1.0, std::fabs(planned.x * cam.x + planned.y * cam.y + planned.z * cam.z +
							      planned.w * cam.w))) *
			    57.29577951308232;
		shown_.push_back({T, cam});
		while (shown_.size() > 2 && shown_[1].t < T - 3.0 * h)
			shown_.erase(shown_.begin());
		haveLast_ = true;
		lastT_ = T;
		lastCam_ = cam;
		return cam;
	}

private:
	void Restart()
	{
		plan_.clear();
		shown_.clear();
		haveLast_ = false;
	}
	// Camera the viewer saw at time t: between the shown cameras (or at most h/4 past the last one:
	// the output times move by fractions of a millisecond with the game clock mapping); further
	// after the last one shown (a skipped OBS frame) or before the first: the previous plan's path.
	// false = neither covers t.
	bool FixedAt(double t, double h, Q *out) const
	{
		if (shown_.size() >= 2 && t >= shown_.front().t - 1e-9 && t <= shown_.back().t + 0.25 * h) {
			size_t i = 0;
			while (i + 2 < shown_.size() && shown_[i + 1].t < t)
				i++;
			const PoseSample &a = shown_[i], &b = shown_[i + 1];
			*out = Slerp(a.q, b.q, std::max(0.0, (t - a.t) / (b.t - a.t))); // > 1: at most h/4 past b
			return true;
		}
		return PlanAt(t, out);
	}
	// The previous plan's camera at time t (between its grid points); false outside the plan.
	bool PlanAt(double t, Q *out) const
	{
		if (plan_.size() < 2 || t < plan_.front().t - 1e-9 || t > plan_.back().t + 1e-9)
			return false;
		size_t i = 0;
		while (i + 2 < plan_.size() && plan_[i + 1].t < t)
			i++;
		const PoseSample &a = plan_[i], &b = plan_[i + 1];
		*out = Slerp(a.q, b.q, std::min(1.0, std::max(0.0, (t - a.t) / (b.t - a.t))));
		return true;
	}
	// camera ref*Exp(p), pulled into the head's room if needed
	Q Project(const Image &img, Q ref, Q head, V3 p, double kk, bool *pulled) const
	{
		Q cam = NormalizeQ(Mul(ref, Exp(p)));
		if (Fits(img, head, cam, kk))
			return cam;
		if (pulled)
			*pulled = true;
		// give way toward the head's view direction first (horizon stays level), else toward the head
		Q toward = head;
		if (st_.horizon && upKnown_) {
			bool ok = true;
			const Q headLevel = RemoveRoll(head, up_, &ok);
			if (ok && Fits(img, head, headLevel, kk))
				toward = headLevel;
		}
		const V3 d = Log(Mul(Conj(toward), cam));
		double lo = 0.0, hi = 1.0;
		for (int b = 0; b < 12; b++) {
			const double mid = 0.5 * (lo + hi);
			(Fits(img, head, Mul(toward, Exp(d * mid)), kk) ? lo : hi) = mid;
		}
		return Mul(toward, Exp(d * lo));
	}

	std::vector<PoseSample> s_; // head poses
	std::vector<PoseSample> plan_;  // latest plan (grid points: time, camera)
	std::vector<PoseSample> shown_; // cameras shown (output time, camera)
	bool haveLast_ = false;         // last output (the same OBS frame again -> the same camera)
	double lastT_ = 0;
	Q lastCam_ = {0, 0, 0, 1};
	double fps_ = 30.0;
	std::vector<Q> heads_;
	std::vector<V3> a_, x_, y_, xn_, g_;
	Settings st_;
	V3 up_ = {0, 1, 0};
	bool upKnown_ = false;
};

} // namespace stab
