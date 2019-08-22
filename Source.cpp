#include "Cosmetics.hpp"
#include "VapourSynth.h"
#include "VSHelper.h"

struct FilterData final {
	self(in, static_cast<const VSMap*>(nullptr));
	self(out, static_cast<VSMap*>(nullptr));
	self(api, static_cast<const VSAPI*>(nullptr));
	self(node, static_cast<VSNodeRef*>(nullptr));
	self(vi, static_cast<const VSVideoInfo*>(nullptr));
	self(thresh, 0.);
	self(process, std::array{ false,false,false });
	FilterData() = default;
	FilterData(FilterData&&) = default;
	FilterData(const FilterData&) = default;
	auto operator=(FilterData&&)->decltype(*this) = default;
	auto operator=(const FilterData&)->decltype(*this) = default;
	~FilterData() {
		if (node != nullptr)
			api->freeNode(node);
	}
	auto InitializeSobel() {
		node = api->propGetNode(in, "clip", 0, nullptr);
		vi = api->getVideoInfo(node);
		auto err = 0;
		thresh = api->propGetFloat(in, "thresh", 0, &err);
		if (err)
			thresh = 128.;
		if (thresh < 0. || thresh > 256.) {
			api->setError(out, "ASobel: thresh must be between 0.0 and 256.0 (inclusive).");
			return false;
		}
		thresh /= 256.;
		if (vi->format == nullptr || vi->width == 0 || vi->height == 0 || vi->format->sampleType != stFloat || vi->format->bitsPerSample != 32 || vi->format->colorFamily == cmRGB) {
			api->setError(out, "ASobel: only single precision floating point, not RGB clips with constant format and dimensions supported.");
			return false;
		}
		auto n = vi->format->numPlanes;
		auto m = std::max(api->propNumElements(in, "planes"), 0);
		for (auto& x : process)
			x = m == 0;
		for (auto i : Range{ m }) {
			auto o = api->propGetInt(in, "planes", i, nullptr);
			if (o < 0 || o >= n) {
				api->setError(out, "ASobel: plane index out of range.");
				return false;
			}
			if (process[o]) {
				api->setError(out, "ASobel: plane specified twice.");
				return false;
			}
			process[o] = true;
		}
		return true;
	}
};

auto eval_multi = [](auto action, auto...params) {
	auto results = std::array<double, sizeof...(params)>{};
	auto cursor = results.data();
	auto eval = [&](auto& ycomb, auto p, auto...rest) {
		*cursor = action(p);
		++cursor;
		if constexpr (sizeof...(rest) != 0)
			ycomb(ycomb, rest...);
	};
	if constexpr (sizeof...(params) != 0)
		eval(eval, params...);
	return results;
};

auto zip = [](auto...p) {
	return eval_multi([](auto x) {return x; }, p...);
};

auto sobel = [](auto srcp8, auto dstp8, auto stride, auto width, auto height, auto thresh) {
	auto srcp = reinterpret_cast<const float*>(srcp8);
	auto dstp = reinterpret_cast<float*>(dstp8);
	auto dstp_orig = dstp;
	stride /= sizeof(float);
	srcp += stride;
	dstp += stride;
	for (auto _ : Range{ 1 ,height - 1 }) {
		for (auto x : Range{ 1, width - 1 }) {
			auto [avg_up, avg_down, avg_left, avg_right] = eval_multi(
				[](auto x) {return (x[0] + (x[1] + x[2]) / 2.) / 2.; },
				zip(srcp[x - stride], srcp[x - stride - 1], srcp[x - stride + 1]),
				zip(srcp[x + stride], srcp[x + stride - 1], srcp[x + stride + 1]),
				zip(srcp[x - 1], srcp[x + stride - 1], srcp[x - stride - 1]),
				zip(srcp[x + 1], srcp[x + stride + 1], srcp[x - stride + 1]));
			auto [abs_v, abs_h] = eval_multi([](auto x) {return std::abs(x); }, avg_up - avg_down, avg_left - avg_right);
			auto abs_max = std::max(abs_h, abs_v);
			dstp[x] = static_cast<float>(std::min((abs_v + abs_h + abs_max) * 6., thresh));
		}
		dstp[0] = dstp[1];
		dstp[width - 1] = dstp[width - 2];
		srcp += stride;
		dstp += stride;
	}
	std::memcpy(dstp_orig, dstp_orig + stride, width * sizeof(float));
	std::memcpy(dstp, dstp - stride, width * sizeof(float));
};

auto FilterInit = [](auto in, auto out, auto instanceData, auto node, auto core, auto vsapi) {
	auto d = reinterpret_cast<FilterData*>(*instanceData);
	vsapi->setVideoInfo(d->vi, 1, node);
};

auto aSobelGetFrame = [](auto n, auto activationReason, auto instanceData, auto frameData, auto frameCtx, auto core, auto vsapi) {
	auto d = reinterpret_cast<const FilterData*>(*instanceData);
	auto nullframe = static_cast<const VSFrameRef*>(nullptr);
	if (activationReason == arInitial)
		vsapi->requestFrameFilter(n, d->node, frameCtx);
	else if (activationReason == arAllFramesReady) {
		auto src = vsapi->getFrameFilter(n, d->node, frameCtx);
		auto frames = std::array{
			d->process[0] ? nullframe : src,
			d->process[1] ? nullframe : src,
			d->process[2] ? nullframe : src
		};
		auto planes = std::array{ 0, 1, 2 };
		auto fmt = vsapi->getFrameFormat(src);
		auto dst = vsapi->newVideoFrame2(fmt, vsapi->getFrameWidth(src, 0), vsapi->getFrameHeight(src, 0), frames.data(), planes.data(), src, core);
		for (auto plane : Range{ fmt->numPlanes })
			if (d->process[plane])
				sobel(vsapi->getReadPtr(src, plane), vsapi->getWritePtr(dst, plane), vsapi->getStride(src, plane),
					vsapi->getFrameWidth(src, plane), vsapi->getFrameHeight(src, plane), d->thresh);
			else
				continue;
		vsapi->freeFrame(src);
		return const_cast<decltype(nullframe)>(dst);
	}
	return nullframe;
};

auto FilterFree = [](auto instanceData, auto core, auto vsapi) {
	auto d = reinterpret_cast<FilterData*>(instanceData);
	delete d;
};

auto aSobelCreate = [](auto in, auto out, auto userData, auto core, auto vsapi) {
	auto d = new FilterData{};
	d->in = in;
	d->out = out;
	d->api = vsapi;
	if (auto init_status = d->InitializeSobel(); init_status == false) {
		delete d;
		return;
	}
	vsapi->createFilter(in, out, "ASobel", FilterInit, aSobelGetFrame, FilterFree, fmParallel, 0, d, core);
};

VS_EXTERNAL_API(auto) VapourSynthPluginInit(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin* plugin) {
	configFunc("com.zonked.awarpsharp2", "warpsf", "Warpsharp floating point version", VAPOURSYNTH_API_VERSION, 1, plugin);
	registerFunc("ASobel",
		"clip:clip;"
		"thresh:float:opt;"
		"planes:int[]:opt;"
		, aSobelCreate, 0, plugin);
}