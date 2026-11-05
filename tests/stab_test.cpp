// Proof test for obs-plugin/stabilizer.h (CPU only, no OBS, no GPU).
//  1. Homography: identity (qv == qa, k == 1) is exactly the plain centre 16:9 crop.
//  2. Homography vs. an independent ray calculation for a rotated camera and zoom.
//  3. Look-ahead: hand-like shake (0.3 deg, 6-11 Hz) on a still head -> steady camera.
//  4. 40 deg glance and back: camera smoother than the head, never an edge.
//  5. Keep horizon level: 10 deg head tilt -> camera tilt ~0.
//  6. The crop is the user's value.
// Real head motion: tests/stab_replay.cpp with a recorded poses-*.csv.
// Eye geometry: DCS on VDXR, layer.log 2026-11-05 (2496x2688, FOV L -49 R 45 U 48 D -50 deg).
#include "../obs-plugin/stabilizer.h"

#include <cstdio>
#include <cstring>

using namespace stab;

#define CHECK(c, ...)                                        \
	do {                                                 \
		if (!(c)) {                                  \
			printf("FAIL line %d: ", __LINE__); \
			printf(__VA_ARGS__);                 \
			printf("\n");                        \
			return 1;                            \
		}                                            \
	} while (0)

static const double DEG = 3.14159265358979 / 180.0;

static Image DcsEye()
{
	Image img;
	img.W = 2496;
	img.H = 2688;
	img.cropX = 0;
	img.cropY = 642;
	img.cropW = 2496;
	img.cropH = 1404; // qvd_center_crop_16x9(2496, 2688)
	img.tl = std::tan(-49 * DEG);
	img.tr = std::tan(45 * DEG);
	img.tu = std::tan(48 * DEG);
	img.td = std::tan(-50 * DEG);
	return img;
}

static Q YawPitchRoll(double yaw, double pitch, double roll)
{
	// yaw around +y, pitch around +x, roll around -z (view axis)
	return Mul(Mul(Exp({0, yaw, 0}), Exp({pitch, 0, 0})), Exp({0, 0, roll}));
}

// Independent reference: output pixel -> ray of camera qv -> into qa view -> image (U,V).
static bool RefMap(const Image &img, Q qa, Q qv, double k, double u, double v, double *U, double *V)
{
	const double Sx = img.tr - img.tl, Sy = img.tu - img.td;
	const double cu = (img.cropX + img.cropW * 0.5) / img.W, cv = (img.cropY + img.cropH * 0.5) / img.H;
	const double iu = cu + (u - 0.5) * (double)img.cropW / img.W * k; // image fraction if qv == qa
	const double iv = cv + (v - 0.5) * (double)img.cropH / img.H * k;
	const V3 rayView = {img.tl + iu * Sx, img.tu - iv * Sy, -1.0}; // y up
	const V3 world = Rotate(qv, rayView);
	const V3 inA = Rotate(Conj(qa), world);
	if (inA.z >= 0)
		return false;
	const double X = inA.x / -inA.z, Yup = inA.y / -inA.z;
	*U = (X - img.tl) / Sx;
	*V = (img.tu - Yup) / Sy;
	return true;
}

static double Shake(double t, int axis)
{
	// sum of sines 6..11 Hz, peak about 0.3 deg per axis
	const double f[3][2] = {{6.3, 9.7}, {7.1, 11.0}, {8.2, 6.6}};
	return (0.18 * std::sin(2 * 3.14159265 * f[axis][0] * t + axis) +
		0.12 * std::sin(2 * 3.14159265 * f[axis][1] * t + 2 * axis)) *
	       DEG;
}


int main()
{
	const Image img = DcsEye();
	const V3 up = {0, 1, 0};

	// 1. identity = plain crop
	{
		const Q qa = YawPitchRoll(0.3, -0.1, 0.05);
		const Mat3 h = Homography(img, qa, qa, 1.0);
		double maxErr = 0;
		for (int i = 0; i <= 10; i++)
			for (int j = 0; j <= 10; j++) {
				const double u = i / 10.0, v = j / 10.0;
				double U, V;
				CHECK(Apply(h, u, v, &U, &V), "identity behind camera");
				const double eu = (img.cropX + u * img.cropW) / img.W, ev = (img.cropY + v * img.cropH) / img.H;
				maxErr = std::max(maxErr, std::max(std::fabs(U - eu), std::fabs(V - ev)));
			}
		printf("1. identity homography vs plain crop: max error %.2e (texture fraction)\n", maxErr);
		CHECK(maxErr < 1e-9, "identity is not the plain crop");
		CHECK(Fits(img, qa, qa, 1.0), "plain crop must fit");
	}

	// 2. rotated camera + zoom vs independent ray math
	{
		const Q qa = YawPitchRoll(0.2, 0.05, 0.02);
		const Q qv = Mul(qa, YawPitchRoll(1.5 * DEG, -1.0 * DEG, 2.0 * DEG));
		const double k = 0.93;
		const Mat3 h = Homography(img, qa, qv, k);
		double maxErr = 0;
		for (int i = 0; i <= 20; i++)
			for (int j = 0; j <= 20; j++) {
				double U, V, Ur, Vr;
				CHECK(Apply(h, i / 20.0, j / 20.0, &U, &V) && RefMap(img, qa, qv, k, i / 20.0, j / 20.0, &Ur, &Vr),
				      "mapping");
				maxErr = std::max(maxErr, std::max(std::fabs(U - Ur), std::fabs(V - Vr)));
			}
		printf("2. homography vs independent ray calculation: max error %.2e (= %.4f px)\n", maxErr,
		       maxErr * img.W);
		CHECK(maxErr < 1e-9, "homography differs from ray math");
	}

	// Look-ahead stabilizer as the plugin runs it: frames at 30 fps, camera for the frame 'delay' old.
	auto run = [&](Settings st, double seconds, auto headAt, auto check) -> int {
		LookAhead la;
		la.Configure(st);
		const double fps = 30.0;
		std::vector<double> ts;
		std::vector<Q> qs;
		for (int f = 0; f < (int)(seconds * fps); f++) {
			const double t = f / fps;
			ts.push_back(t);
			qs.push_back(headAt(t));
			la.AddPose(t, qs.back(), up, true);
			const double target = t - st.delay;
			int shown = -1;
			for (int i = (int)ts.size() - 1; i >= 0; i--)
				if (ts[i] <= target + 1e-9) {
					shown = i;
					break;
				}
			if (shown < 0)
				continue;
			bool comp = false;
			const Q cam = la.Camera(img, ts[shown], qs[shown], &comp);
			if (!Fits(img, qs[shown], cam, la.ZoomK())) {
				printf("FAIL: edge visible at %.3f s\n", ts[shown]);
				return 1;
			}
			check(ts[shown], qs[shown], cam);
		}
		return 0;
	};

	// 3. still head with hand-like shake: camera steady
	{
		Settings st;
		st.crop = 25;
		st.delay = 1.0;
		st.smooth = 50;
		const Q base = YawPitchRoll(0.1, -0.2, 0.0);
		double sh = 0, sc = 0;
		int n = 0;
		CHECK(run(st, 12.0, [&](double t) { return Mul(base, YawPitchRoll(Shake(t, 0), Shake(t, 1), Shake(t, 2))); },
			  [&](double t, Q h, Q c) {
				  if (t > 3.0) {
					  sh += std::pow(Angle(base, h), 2);
					  sc += std::pow(Angle(base, c), 2);
					  n++;
				  }
			  }) == 0,
		      "shake run");
		const double rh = std::sqrt(sh / n) / DEG, rc = std::sqrt(sc / n) / DEG;
		printf("3. shake (0.3 deg, 6-11 Hz), crop 25 %%, delay 1 s, smoothness 50: head RMS %.3f deg, camera RMS %.4f deg -> %.0fx steadier\n",
		       rh, rc, rh / rc);
		CHECK(rc * 20.0 < rh, "shake not reduced at least 20x");
	}

	// 4. 40 deg glance and back (look-ahead: smooth, never an edge)
	{
		Settings st;
		st.crop = 25;
		st.delay = 1.0;
		st.smooth = 50;
		auto yawAt = [](double t) {
			if (t < 4.0)
				return 0.0;
			if (t < 4.6)
				return 40 * DEG * (0.5 - 0.5 * std::cos(3.14159265 * (t - 4.0) / 0.6));
			if (t < 6.0)
				return 40 * DEG;
			if (t < 6.6)
				return 40 * DEG * (0.5 + 0.5 * std::cos(3.14159265 * (t - 6.0) / 0.6));
			return 0.0;
		};
		std::vector<double> cy;
		double maxDiff = 0;
		CHECK(run(st, 10.0, [&](double t) { return YawPitchRoll(yawAt(t), 0, 0); },
			  [&](double t, Q h, Q c) {
				  (void)t;
				  const V3 f = Rotate(c, {0, 0, -1});
				  cy.push_back(std::atan2(-f.x, -f.z));
				  maxDiff = std::max(maxDiff, Angle(h, c) / DEG);
			  }) == 0,
		      "glance run");
		double maxAccCam = 0;
		for (size_t i = 2; i < cy.size(); i++)
			maxAccCam = std::max(maxAccCam, std::fabs(cy[i] - 2 * cy[i - 1] + cy[i - 2]) * 900.0 / DEG);
		const double headAcc = 40.0 * std::pow(3.14159265 / 0.6, 2) / 2.0;
		printf("4. 40 deg glance in 0.6 s and back: camera max %.2f deg from the head, max angular accel %.0f deg/s^2 "
		       "(head %.0f)\n",
		       maxDiff, maxAccCam, headAcc);
		CHECK(maxAccCam < headAcc, "camera rougher than the head");
	}

	// 5. keep horizon level: 10 deg head tilt + shake -> camera tilt ~0
	{
		Settings st;
		st.crop = 25;
		st.delay = 1.0;
		st.horizon = true;
		double maxRoll = 0;
		CHECK(run(st, 8.0,
			  [&](double t) {
				  return Mul(YawPitchRoll(0.3, 0.05, 10 * DEG), YawPitchRoll(Shake(t, 0), Shake(t, 1), Shake(t, 2)));
			  },
			  [&](double t, Q, Q c) {
				  if (t > 2.0)
					  maxRoll = std::max(maxRoll, std::fabs(std::asin(Dot(Rotate(c, {1, 0, 0}), up))) / DEG);
			  }) == 0,
		      "horizon run");
		printf("5. horizon level: head tilt 10 deg, camera tilt at most %.4f deg\n", maxRoll);
		CHECK(maxRoll < 0.05, "horizon not level");
	}

	// 6. crop is the user's value, used as set
	{
		Settings st;
		st.crop = 17.5;
		LookAhead la;
		la.Configure(st);
		printf("6. crop setting 17.5 %% -> zoom %.4f\n", la.ZoomK());
		CHECK(std::fabs(la.ZoomK() - 0.825) < 1e-12, "crop not the user's value");
	}

	printf("PASS\n");
	return 0;
}
