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

/* Only overlaps between selected displays count. Two of the three displays, with every primary
 * and every assignment of display IDs:
 * - the DELL and the 4K alone keep their top edges in line, as in points; the built-in they
 *   would overlap is not sent;
 * - the DELL alone with the built-in sits on it, left aligned;
 * - the built-in and the 4K do not touch in points; the DELL connects them, and they must
 *   still form a valid layout. */
static bool checkSelection()
{
	std::array<SDL_DisplayID, 3> ids{ 1, 2, 3 };
	do
	{
		const std::vector<SDL_DisplayID> order(ids.begin(), ids.end());
		const auto builtinId = order[0];
		const auto dellId = order[1];
		const auto uhdId = order[2];
		for (size_t primary = 0; primary < mixed.size(); primary++)
		{
			const auto o = offsets(mixed, order, primary, { dellId, uhdId });
			const auto dell = monitor(o, dellId);
			const auto uhd = monitor(o, uhdId);
			if (!expect(sdl_monitor_layout::valid({ dell, uhd }) && (dell.y == uhd.y),
			            "the DELL and the 4K alone are top aligned"))
			{
				dump(mixed, order, o);
				return false;
			}

			const auto p = offsets(mixed, order, primary, { builtinId, dellId });
			const auto low = monitor(p, builtinId);
			const auto high = monitor(p, dellId);
			if (!expect(sdl_monitor_layout::valid({ low, high }) && (high.x == low.x) &&
			                (high.y + high.height == low.y),
			            "the DELL alone with the built-in sits on it, left aligned"))
			{
				dump(mixed, order, p);
				return false;
			}

			const auto q = offsets(mixed, order, primary, { builtinId, uhdId });
			if (!expect(sdl_monitor_layout::valid({ monitor(q, builtinId), monitor(q, uhdId) }),
			            "the built-in and the 4K form a valid layout through the DELL"))
			{
				dump(mixed, order, q);
				return false;
			}
		}
	} while (std::next_permutation(ids.begin(), ids.end()));
	return true;
}

/* Layouts where placing each display once, from the first display it touches, gives no valid
 * layout; another neighbour or the other alignment has to be tried. Every primary and every
 * assignment of display IDs. */
static bool checkSearch(const char* name, const std::vector<Display>& displays,
                        const std::vector<size_t>& selection)
{
	std::vector<SDL_DisplayID> ids(displays.size());
	for (size_t i = 0; i < ids.size(); i++)
		ids[i] = static_cast<SDL_DisplayID>(i + 1);
	do
	{
		std::vector<SDL_DisplayID> sel;
		for (const auto i : selection)
			sel.push_back(ids[i]);
		for (size_t primary = 0; primary < displays.size(); primary++)
		{
			const auto o = offsets(displays, ids, primary, sel);
			std::vector<rdpMonitor> list;
			for (size_t i = 0; i < displays.size(); i++)
			{
				if (selection.empty() ||
				    (std::find(selection.begin(), selection.end(), i) != selection.end()))
					list.push_back(monitor(o, ids[i]));
			}
			if (!expect(sdl_monitor_layout::valid(list), name))
			{
				dump(displays, ids, o);
				return false;
			}
		}
	} while (std::next_permutation(ids.begin(), ids.end()));
	return true;
}

static bool checkSearches()
{
	/* A 2x primary with two 1x displays side by side below it, only those two selected: both
	 * touch the primary, but they must be placed next to each other. */
	const std::vector<Display> below{ { "2x", { 0, 0, 1920, 1080 }, 3840, 2160 },
		                              { "a", { -960, 1080, 1920, 1080 }, 1920, 1080 },
		                              { "b", { 960, 1080, 1920, 1080 }, 1920, 1080 } };
	/* A 2x display meeting the primary only at a corner. */
	const std::vector<Display> corner{ { "1x", { 0, 0, 1920, 1080 }, 1920, 1080 },
		                               { "right", { 1920, 0, 1920, 1080 }, 1920, 1080 },
		                               { "2x", { -1920, -1080, 1920, 1080 }, 3840, 2160 } };
	/* A 2x display below the primary has to move left so that a later display fits. */
	const std::vector<Display> blocked{ { "1x", { 0, 0, 1920, 1080 }, 1920, 1080 },
		                                { "portrait", { 1920, -840, 1080, 1920 }, 1080, 1920 },
		                                { "right", { 3000, 540, 1920, 1080 }, 1920, 1080 },
		                                { "2x", { 0, 1080, 1920, 1080 }, 3840, 2160 } };
	return checkSearch("two selected displays below an unselected 2x one stay connected", below,
	                   { 1, 2 }) &&
	       checkSearch("a 2x display at a corner does not overlap", corner, {}) &&
	       checkSearch("an earlier display moves so that a later one fits", blocked, {});
}

/* Two 2x displays right of a 1x one, both touching it and each other: when the second one
 * cannot be placed from the 1x display without overlapping the first, the first places it. */
static bool checkSiblings()
{
	const std::vector<Display> siblings{ { "1x", { 0, 0, 1920, 1080 }, 1920, 1080 },
		                                 { "upper", { 1920, -540, 1920, 1080 }, 3840, 2160 },
		                                 { "lower", { 1920, 540, 1920, 1080 }, 3840, 2160 } };
	std::array<SDL_DisplayID, 3> ids{ 1, 2, 3 };
	do
	{
		const std::vector<SDL_DisplayID> order(ids.begin(), ids.end());
		for (size_t primary = 0; primary < siblings.size(); primary++)
		{
			const auto o = offsets(siblings, order, primary);
			if (!expect(sdl_monitor_layout::valid(
			                { monitor(o, order[0]), monitor(o, order[1]), monitor(o, order[2]) }),
			            "displays that touch the same display and each other do not overlap"))
			{
				dump(siblings, order, o);
				return false;
			}
		}
	} while (std::next_permutation(ids.begin(), ids.end()));
	return true;
}

/* Displays that do not touch the primary along an edge (here only at a corner) are placed at
 * their point offset, which equal scales keep exactly. */
static bool checkUnreached()
{
	const std::vector<Display> corner{ { "primary", { 0, 0, 1920, 1080 }, 1920, 1080 },
		                               { "a", { 1920, 1080, 1920, 1080 }, 1920, 1080 },
		                               { "b", { 3840, 1080, 1920, 1080 }, 1920, 1080 } };
	std::array<SDL_DisplayID, 3> ids{ 1, 2, 3 };
	do
	{
		const std::vector<SDL_DisplayID> order(ids.begin(), ids.end());
		const auto o = offsets(corner, order, 0, { order[1], order[2] });
		const auto a = monitor(o, order[1]);
		const auto b = monitor(o, order[2]);
		if (!expect(sdl_monitor_layout::valid({ a, b }) && (a.x == 1920) && (a.y == 1080) &&
		                (b.x == 3840) && (b.y == 1080),
		            "displays not reached from the primary keep their point layout"))
		{
			dump(corner, order, o);
			return false;
		}
	} while (std::next_permutation(ids.begin(), ids.end()));

	sdl_display_offsets::Offsets none;
	sdl_display_offsets::update(none, 1);

	/* A primary that is not in the list: the first display takes its place. */
	const std::vector<SDL_DisplayID> abc{ 1, 2, 3 };
	sdl_display_offsets::Offsets missing;
	for (size_t i = 0; i < corner.size(); i++)
		missing.emplace(abc[i], std::pair{ corner[i].points,
		                                   SDL_Rect{ 0, 0, corner[i].width, corner[i].height } });
	sdl_display_offsets::update(missing, 42);
	return expect((monitor(missing, 2).x == 1920) && (monitor(missing, 3).x == 3840),
	              "a missing primary is replaced by the first display");
}

int main()
{
	if (!checkMixed() || !checkSelection() || !checkSearches() || !checkSiblings() ||
	    !checkUnreached())
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
