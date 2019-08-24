#include "Cosmetics.hpp"
#include "VapourSynth.h"
#include "VSHelper.h"

struct FilterData final {
	self(filterName, "");
	self(in, static_cast<const VSMap*>(nullptr));
	self(out, static_cast<VSMap*>(nullptr));
	self(api, static_cast<const VSAPI*>(nullptr));
	self(node, static_cast<VSNodeRef*>(nullptr));
	self(mask, static_cast<VSNodeRef*>(nullptr));
	self(vi, static_cast<const VSVideoInfo*>(nullptr));
	self(thresh, 0.);
	self(blur_type, 0ll);
	self(blur_level, 0ll);
	self(depth, std::array{ 0ll,0ll,0ll });
	self(warpAlongLuma, false);
	self(process, std::array{ false,false,false });
	FilterData() = default;
	FilterData(FilterData&&) = default;
	FilterData(const FilterData&) = default;
	auto operator=(FilterData&&)->decltype(*this) = default;
	auto operator=(const FilterData&)->decltype(*this) = default;
	~FilterData() {
		if (node != nullptr)
			api->freeNode(node);
		if (mask != nullptr)
			api->freeNode(mask);
	}
	auto CheckFormat() {
		auto errmsg = filterName + ": only single precision floating point, not RGB clips with constant format and dimensions supported."s;
		if (vi->format == nullptr || vi->width == 0 || vi->height == 0 || vi->format->sampleType != stFloat || vi->format->bitsPerSample != 32 || vi->format->colorFamily == cmRGB) {
			api->setError(out, errmsg.data());
			return false;
		}
		return true;
	}
	auto CheckPlanes() {
		auto n = vi->format->numPlanes;
		auto m = std::max(api->propNumElements(in, "planes"), 0);
		auto errmsg1 = filterName + ": plane index out of range."s;
		auto errmsg2 = filterName + ": plane specified twice."s;
		for (auto& x : process)
			x = m == 0;
		for (auto i : Range{ m }) {
			auto o = api->propGetInt(in, "planes", i, nullptr);
			if (o < 0 || o >= n) {
				api->setError(out, errmsg1.data());
				return false;
			}
			if (process[o]) {
				api->setError(out, errmsg2.data());
				return false;
			}
			process[o] = true;
		}
		return true;
	}
	auto InitializeSobel() {
		filterName = "ASobel";
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
		if (auto format_status = CheckFormat(); format_status == false)
			return false;
		if (auto plane_status = CheckPlanes(); plane_status == false)
			return false;
		return true;
	}
	auto InitializeBlur() {
		filterName = "ABlur";
		node = api->propGetNode(in, "clip", 0, nullptr);
		vi = api->getVideoInfo(node);
		auto err = 0;
		blur_type = api->propGetInt(in, "type", 0, &err);
		if (err)
			blur_type = 1;
		blur_level = api->propGetInt(in, "blur", 0, &err);
		if (err)
			blur_level = blur_type == 1 ? 3 : 2;
		if (blur_level < 0) {
			api->setError(out, "ABlur: blur must be at least 0.");
			return false;
		}
		if (blur_type < 0 || blur_type > 1) {
			api->setError(out, "ABlur: type must be 0 or 1.");
			return false;
		}
		if (auto format_status = CheckFormat(); format_status == false)
			return false;
		if (auto plane_status = CheckPlanes(); plane_status == false)
			return false;
		return true;
	}
	auto InitializeWarp() {
		filterName = "AWarp";
		node = api->propGetNode(in, "clip", 0, nullptr);
		mask = api->propGetNode(in, "mask", 0, nullptr);
		vi = api->getVideoInfo(mask);
		auto clipvi = api->getVideoInfo(node);
		auto err = 0;
		for (auto i : Range{ 3 }) {
			depth[i] = api->propGetInt(in, "depth", i, &err);
			if (err)
				if (i == 0)
					depth[i] = 3;
				else if (i == 1)
					depth[i] = depth[i - 1] / 2;
				else
					depth[i] = depth[i - 1];
		}
		auto chroma = api->propGetInt(in, "chroma", 0, &err);
		if (err)
			chroma = 0;
		if (chroma < 0 || chroma > 1) {
			api->setError(out, "AWarp: chroma must be 0 or 1.");
			return false;
		}
		warpAlongLuma = chroma == 0;
		for (auto x : depth)
			if (x < -128 || x > 127) {
				api->setError(out, "AWarp: depth must be between -128 and 127 (inclusive).");
				return false;
			}
		if (auto format_status = CheckFormat(); format_status == false)
			return false;
		if (vi->format->subSamplingW > 0 || vi->format->subSamplingH > 0) {
			api->setError(out, "AWarp: clip with subsampled chroma is not supported.");
			return false;
		}
		if (auto not_same_size = vi->width != clipvi->width || vi->height != clipvi->height, not_4x_size = vi->width * 4 != clipvi->width || vi->height * 4 != clipvi->height;
			not_same_size && not_4x_size) {
			api->setError(out, "AWarp: clip can either have the same size as mask, or four times the size of mask in each dimension.");
			return false;
		}
		if (vi->format != clipvi->format) {
			api->setError(out, "AWarp: the two clips must have the same format.");
			return false;
		}
		if (auto plane_status = CheckPlanes(); plane_status == false)
			return false;
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

auto avg_multi = [](auto...pairs) {
	return eval_multi([](auto x) {return (x[0] + x[1]) / 2.; }, pairs...);
};

auto sobel = [](auto srcp8, auto dstp8, auto stride, auto width, auto height, auto thresh) {
	auto srcp = reinterpret_cast<const float*>(srcp8);
	auto dstp = reinterpret_cast<float*>(dstp8);
	auto dstp_orig = dstp;
	stride /= sizeof(float);
	srcp += stride;
	dstp += stride;
	for (auto _ : Range{ 1, height - 1 }) {
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

auto blur_r6 = [](auto mask8, auto temp8, auto stride, auto width, auto height) {
	auto mask = reinterpret_cast<float*>(mask8);
	auto temp = reinterpret_cast<float*>(temp8);
	stride /= sizeof(float);
	auto partial_kernel = [](auto center, auto...pairs) {
		auto [avg12, avg34, avg56] = avg_multi(pairs...);
		auto [avg012, avg3456] = avg_multi(zip(center, avg12), zip(avg34, avg56));
		auto [avg0123456] = avg_multi(zip(avg012, avg3456));
		return static_cast<float>((avg012 + avg0123456) / 2.);
	};
	auto complete_kernel = [](auto center, auto...pairs) {
		auto [avg11, avg22, avg33, avg44, avg55, avg66] = avg_multi(pairs...);
		auto [avg12, avg34, avg56] = avg_multi(zip(avg11, avg22), zip(avg33, avg44), zip(avg55, avg66));
		auto [avg012, avg3456] = avg_multi(zip(center, avg12), zip(avg34, avg56));
		auto [avg0123456] = avg_multi(zip(avg012, avg3456));
		return static_cast<float>((avg012 + avg0123456) / 2.);
	};
	auto blurH = [=]() mutable {
		for (auto _ : Range{ height }) {
			for (auto x : Range{ 6 })
				temp[x] = partial_kernel(mask[x], zip(mask[x + 1], mask[x + 2]), zip(mask[x + 3], mask[x + 4]), zip(mask[x + 5], mask[x + 6]));
			for (auto x : Range{ 6, width - 6 })
				temp[x] = complete_kernel(mask[x], zip(mask[x - 1], mask[x + 1]), zip(mask[x - 2], mask[x + 2]), zip(mask[x - 3], mask[x + 3]),
					zip(mask[x - 4], mask[x + 4]), zip(mask[x - 5], mask[x + 5]), zip(mask[x - 6], mask[x + 6]));
			for (auto x : Range{ width - 6, width })
				temp[x] = partial_kernel(mask[x], zip(mask[x - 1], mask[x - 2]), zip(mask[x - 3], mask[x - 4]), zip(mask[x - 5], mask[x - 6]));
			mask += stride;
			temp += stride;
		}
	};
	auto blurV = [=]() mutable {
		for (auto _ : Range{ 6 }) {
			for (auto x : Range{ width })
				mask[x] = partial_kernel(temp[x], zip(temp[x + stride], temp[x + stride * 2]), zip(temp[x + stride * 3], temp[x + stride * 4]), zip(temp[x + stride * 5], temp[x + stride * 6]));
			mask += stride;
			temp += stride;
		}
		for (auto _ : Range{ 6, height - 6 }) {
			for (auto x : Range{ width })
				mask[x] = complete_kernel(temp[x], zip(temp[x - stride], temp[x + stride]), zip(temp[x - stride * 2], temp[x + stride * 2]), zip(temp[x - stride * 3], temp[x + stride * 3]),
					zip(temp[x - stride * 4], temp[x + stride * 4]), zip(temp[x - stride * 5], temp[x + stride * 5]), zip(temp[x - stride * 6], temp[x + stride * 6]));
			mask += stride;
			temp += stride;
		}
		for (auto _ : Range{ height - 6, height }) {
			for (auto x : Range{ width })
				mask[x] = partial_kernel(temp[x], zip(temp[x - stride], temp[x - stride * 2]), zip(temp[x - stride * 3], temp[x - stride * 4]), zip(temp[x - stride * 5], temp[x - stride * 6]));
			mask += stride;
			temp += stride;
		}
	};
	blurH();
	blurV();
};

auto blur_r2 = [](auto mask8, auto temp8, auto stride, auto width, auto height) {
	auto mask = reinterpret_cast<float*>(mask8);
	auto temp = reinterpret_cast<float*>(temp8);
	stride /= sizeof(float);
	auto kernel = [](auto center, auto...pairs) {
		auto [avg1, avg2] = avg_multi(pairs...);
		auto avg = (avg2 + 3. * center) / 4.;
		return static_cast<float>((avg + avg1) / 2.);
	};
	auto blurH = [=]() mutable {
		for (auto _ : Range{ height }) {
			temp[0] = kernel(mask[0], zip(mask[0], mask[1]), zip(mask[0], mask[2]));
			temp[1] = kernel(mask[1], zip(mask[0], mask[2]), zip(mask[0], mask[3]));
			for (auto x : Range{ 2, width - 2 })
				temp[x] = kernel(mask[x], zip(mask[x - 1], mask[x + 1]), zip(mask[x - 2], mask[x + 2]));
			temp[width - 2] = kernel(mask[width - 2], zip(mask[width - 3], mask[width - 1]), zip(mask[width - 4], mask[width - 1]));
			temp[width - 1] = kernel(mask[width - 1], zip(mask[width - 2], mask[width - 1]), zip(mask[width - 3], mask[width - 1]));
			mask += stride;
			temp += stride;
		}
	};
	auto blurV = [=]() mutable {
		for (auto y : Range{ height }) {
			auto stride_p1 = y > 0 ? -stride : 0;
			auto stride_p2 = y > 1 ? stride_p1 * 2 : stride_p1;
			auto stride_n1 = y < height - 1 ? stride : 0;
			auto stride_n2 = y < height - 2 ? stride_n1 * 2 : stride_n1;
			for (auto x : Range{ width })
				mask[x] = kernel(temp[x], zip(temp[x + stride_p1], temp[x + stride_n1]), zip(temp[x + stride_p2], temp[x + stride_n2]));
			mask += stride;
			temp += stride;
		}
	};
	blurH();
	blurV();
};

auto warp = [](auto srcp8, auto edgep8, auto dstp8, auto src_stride, auto edge_stride, auto dst_stride, auto width, auto height, auto depth, auto SMAGL) {
	auto srcp = reinterpret_cast<const float*>(srcp8);
	auto edgep = reinterpret_cast<const float*>(edgep8);
	auto dstp = reinterpret_cast<float*>(dstp8);
	auto SMAG = 1 << SMAGL;
	auto x_limit_max = static_cast<long long>(width - 1)* SMAG;
	src_stride /= sizeof(float);
	edge_stride /= sizeof(float);
	dst_stride /= sizeof(float);
	depth <<= 8;
	for (auto y : Range{ height }) {
		for (auto x : Range{ width }) {
			auto above = y == 0 ? edgep[x] : edgep[-edge_stride + x];
			auto below = y == height - 1 ? edgep[x] : edgep[edge_stride + x];
			auto left = x == 0 ? edgep[x] : edgep[x - 1];
			auto right = x == width - 1 ? edgep[x] : edgep[x + 1];
			auto calc_hv = [=](auto x) {
				auto scaled = static_cast<long long>(nearbyintl(x * 256.));
				scaled <<= 7;
				scaled *= depth;
				return scaled >> 16;
			};
			auto calc_remainder = [=](auto x) {
				if (SMAGL != 0)
					x <<= SMAGL;
				return static_cast<double>(x & 127);
			};
			auto weighted_avg = [](auto x) {return (x[0] * (128. - x[2]) + x[1] * x[2]) / 128.; };
			auto h = calc_hv(left - right), v = calc_hv(above - below);
			v = std::min(std::max(v, -y * 128ll), (height - y) * 128ll - 129);
			auto [remainder_h, remainder_v] = eval_multi(calc_remainder, h, v);
			h >>= 7 - SMAGL;
			v >>= 7 - SMAGL;
			h += x << SMAGL;
			if (auto remainder_needed = (x_limit_max > h) && !(h < 0); remainder_needed == false)
				remainder_h = 0.;
			h = std::max(std::min(h, x_limit_max), 0ll);
			auto [s0, s1] = eval_multi(weighted_avg, zip(srcp[v * src_stride + h], srcp[v * src_stride + h + 1], remainder_h),
				zip(srcp[(v + 1) * src_stride + h], srcp[(v + 1) * src_stride + h + 1], remainder_h));
			dstp[x] = static_cast<float>(weighted_avg(zip(s0, s1, remainder_v)));
		}
		srcp += src_stride * SMAG;
		edgep += edge_stride;
		dstp += dst_stride;
	}
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

auto aBlurGetFrame = [](auto n, auto activationReason, auto instanceData, auto frameData, auto frameCtx, auto core, auto vsapi) {
	auto d = reinterpret_cast<const FilterData*>(*instanceData);
	auto nullframe = static_cast<const VSFrameRef*>(nullptr);
	if (activationReason == arInitial)
		vsapi->requestFrameFilter(n, d->node, frameCtx);
	else if (activationReason == arAllFramesReady) {
		auto src = vsapi->getFrameFilter(n, d->node, frameCtx);
		auto dst = vsapi->copyFrame(src, core);
		auto fmt = vsapi->getFrameFormat(dst);
		auto temp_stride = static_cast<std::size_t>(vsapi->getStride(dst, d->process[0] ? 0 : 1));
		auto temp_height = vsapi->getFrameHeight(dst, d->process[0] ? 0 : 1);
		auto temp = vs_aligned_malloc(temp_stride * temp_height, 32);
		auto blur_level = std::array{ d->blur_level, (d->blur_level + 1) / 2, (d->blur_level + 1) / 2 };
		vsapi->freeFrame(src);
		for (auto plane : Range{ fmt->numPlanes })
			if (d->process[plane])
				for (auto _ : Range{ blur_level[plane] })
					if (d->blur_type == 0)
						blur_r6(vsapi->getWritePtr(dst, plane), temp, vsapi->getStride(dst, plane), vsapi->getFrameWidth(dst, plane), vsapi->getFrameHeight(dst, plane));
					else
						blur_r2(vsapi->getWritePtr(dst, plane), temp, vsapi->getStride(dst, plane), vsapi->getFrameWidth(dst, plane), vsapi->getFrameHeight(dst, plane));
			else
				continue;
		vs_aligned_free(temp);
		return const_cast<decltype(nullframe)>(dst);
	}
	return nullframe;
};

auto aWarpGetFrame = [](auto n, auto activationReason, auto instanceData, auto frameData, auto frameCtx, auto core, auto vsapi) {
	auto d = reinterpret_cast<const FilterData*>(*instanceData);
	auto nullframe = static_cast<const VSFrameRef*>(nullptr);
	if (activationReason == arInitial) {
		vsapi->requestFrameFilter(n, d->node, frameCtx);
		vsapi->requestFrameFilter(n, d->mask, frameCtx);
	}
	else if (activationReason == arAllFramesReady) {
		auto src = vsapi->getFrameFilter(n, d->node, frameCtx);
		auto mask = vsapi->getFrameFilter(n, d->mask, frameCtx);
		auto SMAGL = 0;
		auto frames = std::array{
			d->process[0] ? nullframe : src,
			d->process[1] ? nullframe : src,
			d->process[2] ? nullframe : src
		};
		auto planes = std::array{ 0, 1, 2 };
		auto fmt = vsapi->getFrameFormat(src);
		auto src_width = vsapi->getFrameWidth(src, 0);
		auto mask_width = vsapi->getFrameWidth(mask, 0);
		if (mask_width != src_width) {
			for (auto& x : frames)
				x = nullframe;
			SMAGL = 2;
		}
		auto dst = vsapi->newVideoFrame2(fmt, mask_width, vsapi->getFrameHeight(mask, 0), frames.data(), planes.data(), src, core);
		for (auto plane : Range{ fmt->numPlanes })
			if (d->process[plane])
				warp(vsapi->getReadPtr(src, plane), vsapi->getReadPtr(mask, d->warpAlongLuma ? 0 : plane), vsapi->getWritePtr(dst, plane), vsapi->getStride(src, plane),
					vsapi->getStride(mask, plane), vsapi->getStride(dst, plane), vsapi->getFrameWidth(dst, plane), vsapi->getFrameHeight(dst, plane), d->depth[plane], SMAGL);
			else
				continue;
		vsapi->freeFrame(src);
		vsapi->freeFrame(mask);
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

auto aBlurCreate = [](auto in, auto out, auto userData, auto core, auto vsapi) {
	auto d = new FilterData{};
	d->in = in;
	d->out = out;
	d->api = vsapi;
	if (auto init_status = d->InitializeBlur(); init_status == false) {
		delete d;
		return;
	}
	vsapi->createFilter(in, out, "ABlur", FilterInit, aBlurGetFrame, FilterFree, fmParallel, 0, d, core);
};

auto aWarpCreate = [](auto in, auto out, auto userData, auto core, auto vsapi) {
	auto d = new FilterData{};
	d->in = in;
	d->out = out;
	d->api = vsapi;
	if (auto init_status = d->InitializeWarp(); init_status == false) {
		delete d;
		return;
	}
	vsapi->createFilter(in, out, "AWarp", FilterInit, aWarpGetFrame, FilterFree, fmParallel, 0, d, core);
};

VS_EXTERNAL_API(auto) VapourSynthPluginInit(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin* plugin) {
	configFunc("com.zonked.awarpsharp2", "warpsf", "Warpsharp floating point version", VAPOURSYNTH_API_VERSION, 1, plugin);
	registerFunc("ASobel",
		"clip:clip;"
		"thresh:float:opt;"
		"planes:int[]:opt;"
		, aSobelCreate, 0, plugin);
	registerFunc("ABlur",
		"clip:clip;"
		"blur:int:opt;"
		"type:int:opt;"
		"planes:int[]:opt;"
		, aBlurCreate, 0, plugin);
	registerFunc("AWarp",
		"clip:clip;"
		"mask:clip;"
		"depth:int[]:opt;"
		"chroma:int:opt;"
		"planes:int[]:opt;"
		, aWarpCreate, 0, plugin);
}