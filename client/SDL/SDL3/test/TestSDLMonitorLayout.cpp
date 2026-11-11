/** Multimon layout compaction for fullscreen windows shorter than their displays. */
#include "../sdl_monitor_layout.hpp"

#include <cstdio>
#include <vector>

static bool expect(bool value, const char* message)
{
	if (!value)
		(void)fprintf(stderr, "%s\n", message);
	return value;
}

static rdpMonitor mon(INT32 x, INT32 y, INT32 width, INT32 height)
{
	rdpMonitor m = {};
	m.x = x;
	m.y = y;
	m.width = width;
	m.height = height;
	return m;
}

static bool at(const rdpMonitor& m, INT32 x, INT32 y)
{
	return (m.x == x) && (m.y == y);
}

/* Effective rectangles and the display rectangles they came from; the effective ones start at
 * the display origins. */
struct Layout
{
	std::vector<rdpMonitor> list;
	std::vector<rdpMonitor> displays;
};

static Layout layout(const std::vector<rdpMonitor>& displays, size_t shortened, INT32 lost)
{
	Layout l{ displays, displays };
	l.list[shortened].height -= lost;
	return l;
}

int main()
{
	if (!expect(sdl_monitor_layout::valid({ mon(0, 0, 1024, 834), mon(0, 834, 1024, 768),
	                                        mon(1024, 1570, 1024, 768) }),
	            "the review's three-monitor display layout is valid"))
		return 1;

	// Two monitors stacked below a display whose window lost 66 rows to the notch.
	auto stacked = layout({ mon(0, 0, 1024, 834), mon(0, 834, 1024, 768) }, 0, 66);
	if (!expect(sdl_close_monitor_gaps(stacked.list, stacked.displays) &&
	                at(stacked.list[0], 0, 0) && at(stacked.list[1], 0, 768),
	            "the monitor below the shortened one moves up to touch it"))
		return 1;

	// Side by side: the shortened height leaves no gap, nothing moves.
	auto side = layout({ mon(0, 0, 1024, 834), mon(1024, -120, 1920, 1080) }, 0, 66);
	if (!expect(sdl_close_monitor_gaps(side.list, side.displays) && at(side.list[0], 0, 0) &&
	                at(side.list[1], 1024, -120),
	            "side-by-side monitors keep their positions"))
		return 1;

	// From the PR #8 review: B moves up and then shares D's vertical range, so D must not move
	// left into B. D touched C, which stays, so D stays as well.
	auto staggered = layout({ mon(0, 0, 1024, 834), mon(200, 834, 1024, 768),
	                          mon(1024, 0, 1024, 417), mon(1424, 417, 1024, 417) },
	                        0, 66);
	if (!expect(sdl_close_monitor_gaps(staggered.list, staggered.displays),
	            "the staggered layout is compacted to a valid layout") ||
	    !expect(at(staggered.list[0], 0, 0) && at(staggered.list[1], 200, 768) &&
	                at(staggered.list[2], 1024, 0) && at(staggered.list[3], 1424, 417),
	            "in the staggered layout only the monitor below the shortened one moves"))
		return 1;

	// From the PR #8 review: C only touches B on its side, with 32 rows. Moving B up to the
	// shortened A would need C to move as well; keeping A's bottom edge moves only A.
	auto three = layout({ mon(0, 0, 1024, 834), mon(0, 834, 1024, 768),
	                      mon(1024, 1570, 1024, 768) },
	                    0, 66);
	if (!expect(sdl_close_monitor_gaps(three.list, three.displays),
	            "the three-monitor layout is compacted to a valid layout") ||
	    !expect(at(three.list[0], 0, 66) && at(three.list[1], 0, 834) &&
	                at(three.list[2], 1024, 1570),
	            "the shortened monitor keeps its bottom edge so its side neighbor stays attached"))
		return 1;

	// The same when only the top arrangement is possible: something above the shortened A keeps
	// its top edge in place, so B and its side neighbor C move up together.
	auto held = layout({ mon(0, -1080, 1024, 1080), mon(0, 0, 1024, 834), mon(0, 834, 1024, 768),
	                     mon(1024, 1570, 1024, 768) },
	                   1, 66);
	if (!expect(sdl_close_monitor_gaps(held.list, held.displays) && at(held.list[0], 0, -1080) &&
	                at(held.list[1], 0, 0) && at(held.list[2], 0, 768) &&
	                at(held.list[3], 1024, 1504),
	            "the side neighbor follows the monitor it touches"))
		return 1;

	// A side neighbor that only touched the rows the shortened monitor lost follows its bottom.
	auto band = layout({ mon(0, 0, 1024, 834), mon(1024, 813, 1024, 768) }, 0, 66);
	if (!expect(sdl_close_monitor_gaps(band.list, band.displays) && at(band.list[0], 0, 0) &&
	                at(band.list[1], 1024, 747),
	            "a contact in the lost rows is kept by following the bottom edge"))
		return 1;

	// B below A cannot move up because C, which B also touches, holds it; A keeps its bottom.
	auto pinned = layout({ mon(0, 0, 1024, 834), mon(861, 834, 2560, 1440),
	                       mon(1581, -246, 1920, 1080) },
	                     0, 66);
	if (!expect(sdl_close_monitor_gaps(pinned.list, pinned.displays) &&
	                at(pinned.list[0], 0, 66) && at(pinned.list[1], 861, 834) &&
	                at(pinned.list[2], 1581, -246),
	            "a monitor that cannot close the gap is reached from the shortened one"))
		return 1;

	// Two monitors below a wide shortened display: moving the shortened one moves least.
	auto pair = layout({ mon(0, 0, 2048, 834), mon(0, 834, 1024, 768),
	                     mon(1024, 834, 1024, 768) },
	                   0, 66);
	if (!expect(sdl_close_monitor_gaps(pair.list, pair.displays) && at(pair.list[0], 0, 66) &&
	                at(pair.list[1], 0, 834) && at(pair.list[2], 1024, 834),
	            "the arrangement that moves the monitors least is used"))
		return 1;

	// A monitor right of the moved one that also touches a fixed one above it stays.
	auto row = layout({ mon(0, 0, 1024, 834), mon(1024, 0, 1024, 834), mon(0, 834, 1024, 768),
	                    mon(1024, 834, 1024, 768) },
	                  0, 66);
	if (!expect(sdl_close_monitor_gaps(row.list, row.displays) && at(row.list[2], 0, 768) &&
	                at(row.list[3], 1024, 834),
	            "a monitor below an unchanged display stays where it is"))
		return 1;

	// A display above the shortened one and one left of it: nothing below, nothing moves.
	auto above = layout({ mon(0, -1080, 1920, 1080), mon(0, 0, 1024, 834),
	                      mon(-1920, 0, 1920, 1080) },
	                    1, 66);
	if (!expect(sdl_close_monitor_gaps(above.list, above.displays) &&
	                at(above.list[0], 0, -1080) && at(above.list[1], 0, 0) &&
	                at(above.list[2], -1920, 0),
	            "monitors above and beside the shortened one keep their positions"))
		return 1;

	// Without display rectangles the positions are kept and only checked.
	std::vector<rdpMonitor> apart{ mon(0, 0, 100, 100), mon(500, 500, 100, 100) };
	if (!expect(!sdl_close_monitor_gaps(apart, {}) && at(apart[0], 0, 0) &&
	                at(apart[1], 500, 500),
	            "an unrelated layout is kept and reported as not valid"))
		return 1;

	// The validity check itself.
	if (!expect(sdl_monitor_layout::overlapping({ mon(0, 0, 100, 100), mon(50, 50, 100, 100) }),
	            "overlapping monitors are detected") ||
	    !expect(!sdl_monitor_layout::valid({ mon(0, 0, 100, 100), mon(100, 100, 100, 100) }),
	            "corner contact is not adjacency") ||
	    !expect(sdl_monitor_layout::valid({ mon(0, 0, 100, 100), mon(100, 50, 100, 100) }),
	            "a shared edge segment is adjacency"))
		return 1;

	// Every arrangement of three displays on a grid, with any one of them shortened, stays
	// free of overlaps and keeps every monitor adjacent when the displays were.
	const std::vector<rdpMonitor> cells{ mon(0, 0, 1024, 834),     mon(1024, 0, 1024, 834),
		                                 mon(0, 834, 1024, 834),   mon(1024, 834, 1024, 834),
		                                 mon(512, 1668, 1024, 834), mon(2048, 400, 1024, 834) };
	size_t checked = 0;
	for (size_t a = 0; a < cells.size(); a++)
		for (size_t b = a + 1; b < cells.size(); b++)
			for (size_t c = b + 1; c < cells.size(); c++)
			{
				const std::vector<rdpMonitor> displays{ cells[a], cells[b], cells[c] };
				if (!sdl_monitor_layout::valid(displays))
					continue;
				for (size_t s = 0; s < displays.size(); s++)
				{
					auto l = layout(displays, s, 66);
					checked++;
					if (!expect(sdl_close_monitor_gaps(l.list, l.displays),
					            "a valid three-display grid layout stays valid"))
					{
						(void)fprintf(stderr, "cells %zu %zu %zu, shortened %zu\n", a, b, c, s);
						return 1;
					}
				}
			}
	(void)printf("checked %zu grid layouts\n", checked);
	return 0;
}
