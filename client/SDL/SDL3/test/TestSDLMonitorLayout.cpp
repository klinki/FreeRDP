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

int main()
{
	// Two monitors stacked below a display whose window lost 66 rows to the notch.
	std::vector<rdpMonitor> stacked{ mon(0, 0, 1024, 768), mon(0, 834, 1024, 768) };
	if (!expect(sdl_close_monitor_gaps(stacked) && at(stacked[0], 0, 0) &&
	                at(stacked[1], 0, 768) && sdl_monitor_layout::valid(stacked),
	            "the monitor below the shortened one moves up to touch it"))
		return 1;

	// Side by side: the shortened height leaves no gap, nothing moves.
	std::vector<rdpMonitor> side{ mon(0, 0, 1024, 768), mon(1024, -120, 1920, 1080) };
	if (!expect(sdl_close_monitor_gaps(side) && at(side[0], 0, 0) && at(side[1], 1024, -120),
	            "side-by-side monitors keep their positions"))
		return 1;

	// The staggered layout from the PR #8 review: B moves up in the vertical pass and then
	// shares D's vertical range, so the horizontal pass must not move D into B.
	std::vector<rdpMonitor> staggered{ mon(0, 0, 1024, 768), mon(200, 834, 1024, 768),
		                               mon(1024, 0, 1024, 417), mon(1424, 417, 1024, 417) };
	if (!expect(sdl_close_monitor_gaps(staggered), "the staggered layout is compacted") ||
	    !expect(!sdl_monitor_layout::overlapping(staggered),
	            "compacting the staggered layout creates no overlap") ||
	    !expect(sdl_monitor_layout::valid(staggered),
	            "every staggered monitor is adjacent to another") ||
	    !expect(at(staggered[0], 0, 0) && at(staggered[1], 200, 768) &&
	                at(staggered[2], 1024, 0) && at(staggered[3], 1224, 417),
	            "the staggered monitors close their gaps without crossing each other"))
		return 1;

	// A monitor right of a shortened one only shifts left when the row was also its row.
	std::vector<rdpMonitor> row{ mon(0, 0, 1024, 768), mon(0, 834, 1024, 768),
		                         mon(1024, 834, 1024, 768) };
	if (!expect(sdl_close_monitor_gaps(row) && at(row[1], 0, 768) && at(row[2], 1024, 834) &&
	                sdl_monitor_layout::valid(row),
	            "a monitor next to the moved one stays where it is"))
		return 1;

	// Monitors that share no range on either axis have nothing to close towards and stay put.
	std::vector<rdpMonitor> apart{ mon(0, 0, 100, 100), mon(500, 500, 100, 100) };
	if (!expect(sdl_close_monitor_gaps(apart) && at(apart[0], 0, 0) && at(apart[1], 500, 500),
	            "unrelated monitors keep their positions"))
		return 1;

	// The validity check itself.
	if (!expect(sdl_monitor_layout::overlapping({ mon(0, 0, 100, 100), mon(50, 50, 100, 100) }),
	            "overlapping monitors are detected") ||
	    !expect(!sdl_monitor_layout::valid({ mon(0, 0, 100, 100), mon(100, 100, 100, 100) }),
	            "corner contact is not adjacency") ||
	    !expect(sdl_monitor_layout::valid({ mon(0, 0, 100, 100), mon(100, 50, 100, 100) }),
	            "a shared edge segment is adjacency"))
		return 1;
	return 0;
}
