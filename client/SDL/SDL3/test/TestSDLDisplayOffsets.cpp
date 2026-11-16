/** Pixel origins of local displays with mixed scales (sheltie-app/sheltie-macos#94). */
#include "../sdl_display_offsets.hpp"
#include "../sdl_monitor_layout.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

static bool expect(bool value, const char* message)
{
	if (!value)
		(void)fprintf(stderr, "FAIL: %s\n", message);
	return value;
}

struct Display
{
	const char* name;
	SDL_Rect points;
	int width;
	int height;
};

static sdl_display_offsets::Offsets offsets(const std::vector<Display>& displays,
                                            const std::vector<SDL_DisplayID>& ids, size_t primary,
                                            const std::vector<SDL_DisplayID>& selection = {})
{
	/* SDL reports bounds relative to the primary display, which starts at (0, 0). */
	const auto origin = displays[primary].points;
	sdl_display_offsets::Offsets result;
	for (size_t i = 0; i < displays.size(); i++)
	{
		auto bounds = displays[i].points;
		bounds.x -= origin.x;
		bounds.y -= origin.y;
		SDL_Rect pixel{ 0, 0, displays[i].width, displays[i].height };
		result.emplace(ids[i], std::pair{ bounds, pixel });
	}
	sdl_display_offsets::update(result, ids[primary], selection);
	return result;
}

static rdpMonitor monitor(const sdl_display_offsets::Offsets& o, SDL_DisplayID id)
{
	const auto& pixel = o.at(id).second;
	rdpMonitor m = {};
	m.x = pixel.x;
	m.y = pixel.y;
	m.width = pixel.w;
	m.height = pixel.h;
	return m;
}

static bool spans(const rdpMonitor& a, const rdpMonitor& b, INT32 width, INT32 height)
{
	const auto left = std::min(a.x, b.x);
	const auto top = std::min(a.y, b.y);
	const auto right = std::max(a.x + a.width, b.x + b.width);
	const auto bottom = std::max(a.y + a.height, b.y + b.height);
	return (right - left == width) && (bottom - top == height);
}

static void dump(const std::vector<Display>& displays, const std::vector<SDL_DisplayID>& ids,
                 const sdl_display_offsets::Offsets& o)
{
	for (size_t i = 0; i < displays.size(); i++)
	{
		const auto m = monitor(o, ids[i]);
		(void)fprintf(stderr, "  %-8s id %u at %d,%d %dx%d\n", displays[i].name, ids[i], m.x, m.y,
		              m.width, m.height);
	}
}

/* The maintainer's macOS arrangement in points, relative to the built-in display: the DELL is
 * above the built-in, left aligned, and the 4K is right of the DELL, top aligned. With the 4K as
 * primary this is the second report of #94 (built-in below the DELL); with the built-in as
 * primary it is the original report. */
static const std::vector<Display> mixed{
	{ "built-in", { 0, 0, 1470, 956 }, 2940, 1912 },
	{ "dell", { 0, -1080, 1920, 1080 }, 1920, 1080 },
	{ "4k", { 1920, -1080, 1920, 1080 }, 3840, 2160 },
};

static bool checkMixed()
{
	/* Every assignment of display IDs, since the walk follows the map order of the IDs. */
	std::array<SDL_DisplayID, 3> ids{ 1, 2, 3 };
	do
	{
		const std::vector<SDL_DisplayID> order(ids.begin(), ids.end());
		for (size_t primary = 0; primary < mixed.size(); primary++)
		{
			const auto o = offsets(mixed, order, primary);
			const auto builtin = monitor(o, order[0]);
			const auto dell = monitor(o, order[1]);
			const auto uhd = monitor(o, order[2]);
			const bool all = sdl_monitor_layout::valid({ builtin, dell, uhd });
			const bool pair = sdl_monitor_layout::valid({ dell, uhd });
			const bool size = spans(dell, uhd, 5760, 2160);
			bool placed = true;
			if (primary == 2)
				placed = (dell.x == -1920) && (dell.y == 0) && (uhd.x == 0) && (uhd.y == 0);
			if (!all || !pair || !size || !placed)
			{
				(void)fprintf(stderr, "primary %s, ids %u %u %u:\n", mixed[primary].name, order[0],
				              order[1], order[2]);
				dump(mixed, order, o);
				return expect(all, "all three displays form a valid layout") &&
				       expect(pair, "the DELL and the 4K form a valid layout") &&
				       expect(size, "the DELL and the 4K span 5760x2160") &&
				       expect(placed, "with the 4K as primary the DELL is at -1920,0");
			}
		}
	} while (std::next_permutation(ids.begin(), ids.end()));
	return true;
}

/* Only overlaps between selected displays count. The DELL and the 4K alone keep their top edges
 * in line, as in points; the built-in they would overlap is not sent. */
static bool checkSelection()
{
	const std::vector<SDL_DisplayID> ids{ 1, 2, 3 };
	for (size_t primary = 0; primary < mixed.size(); primary++)
	{
		const auto o = offsets(mixed, ids, primary, { 2, 3 });
		const auto dell = monitor(o, 2);
		const auto uhd = monitor(o, 3);
		if (!expect(sdl_monitor_layout::valid({ dell, uhd }) && (dell.y == uhd.y),
		            "the DELL and the 4K alone are top aligned"))
		{
			dump(mixed, ids, o);
			return false;
		}
	}

	/* With the built-in selected the 4K is bottom aligned with the DELL, so it does not overlap the
	 * built-in and touches it. The built-in and the 4K do not touch in points, so this needs the
	 * built-in or the DELL as primary: from the 4K the built-in is placed under the DELL only. */
	for (size_t primary = 0; primary < 2; primary++)
	{
		const auto o = offsets(mixed, ids, primary, { 1, 3 });
		const auto builtin = monitor(o, 1);
		const auto dell = monitor(o, 2);
		const auto uhd = monitor(o, 3);
		if (!expect(sdl_monitor_layout::valid({ builtin, uhd }) &&
		                (uhd.y + uhd.height == dell.y + dell.height),
		            "with the built-in selected the 4K is bottom aligned with the DELL"))
		{
			dump(mixed, ids, o);
			return false;
		}
	}
	return true;
}

int main()
{
	if (!checkMixed() || !checkSelection())
		return 1;

	/* Same-scale displays side by side and stacked keep their point layout. */
	const std::vector<Display> plain{ { "a", { 0, 0, 1920, 1080 }, 1920, 1080 },
		                              { "b", { 1920, 0, 1920, 1080 }, 1920, 1080 },
		                              { "c", { 0, 1080, 1920, 1080 }, 1920, 1080 } };
	const std::vector<SDL_DisplayID> abc{ 1, 2, 3 };
	auto o = offsets(plain, abc, 0);
	auto b = monitor(o, 2);
	auto c = monitor(o, 3);
	if (!expect((b.x == 1920) && (b.y == 0) && (c.x == 0) && (c.y == 1080),
	            "same-scale displays keep their positions"))
		return 1;

	/* A display above a 2x one, starting inside it: the offset is scaled by the 2x display. */
	const std::vector<Display> inside{ { "retina", { 0, 0, 1470, 956 }, 2940, 1912 },
		                               { "dell", { 500, -1080, 1920, 1080 }, 1920, 1080 } };
	const std::vector<SDL_DisplayID> ab{ 1, 2 };
	o = offsets(inside, ab, 0);
	b = monitor(o, 2);
	if (!expect((b.x == 1000) && (b.y == -1080), "the cross offset uses the reference's scale"))
		return 1;
	/* The same from the other side: the offset still lies on the 2x display, which is now the
	 * one being placed, so the pixel offset is the same. */
	o = offsets(inside, ab, 1);
	const auto a = monitor(o, 1);
	if (!expect((a.x == -1000) && (a.y == 1080), "the cross offset is the same from either side"))
		return 1;

	/* A 1x display that mostly hangs off the side of a 2x one still touches it. */
	const std::vector<Display> hanging{ { "retina", { 0, 0, 1470, 956 }, 2940, 1912 },
		                                { "dell", { -1800, -1080, 1920, 1080 }, 1920, 1080 } };
	o = offsets(hanging, ab, 0);
	if (!expect(sdl_monitor_layout::valid({ monitor(o, 1), monitor(o, 2) }),
	            "a display hanging off the side of a 2x one stays adjacent"))
	{
		dump(hanging, ab, o);
		return 1;
	}

	/* Displays whose edges are in line but do not touch are not neighbours: the far one is placed
	 * from the display it actually touches. */
	const std::vector<Display> corner{ { "a", { 0, 0, 1920, 1080 }, 3840, 2160 },
		                               { "b", { -1920, 0, 1920, 1080 }, 1920, 1080 },
		                               { "c", { -1920, 1080, 1920, 1080 }, 1920, 1080 } };
	const std::vector<SDL_DisplayID> cba{ 3, 2, 1 };
	o = offsets(corner, cba, 0);
	b = monitor(o, 2);
	c = monitor(o, 1);
	if (!expect((b.x == -1920) && (b.y == 0) && (c.x == -1920) && (c.y == 1080),
	            "a display touching only by a corner is not a neighbour"))
	{
		dump(corner, cba, o);
		return 1;
	}
	return 0;
}
