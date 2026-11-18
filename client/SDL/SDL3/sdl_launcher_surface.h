/** Hosted rendering: the Mach messages that carry frames to the launcher
 * (launcher-bridge.md, "Surface channel"). Plain C so that a launcher can include it. */
#pragma once
#include <mach/mach.h>
#include <stdint.h>

enum
{
	SDL_LAUNCHER_SURFACE_COUNT = 3, /* surfaces per window, used in rotation */
	SDL_LAUNCHER_SURFACE_PORT_SLOT = 0, /* registered-port slot holding the launcher's port */
	SDL_LAUNCHER_MSG_SURFACE = 1,
	SDL_LAUNCHER_MSG_FRAME = 2
};

#pragma pack(push, 4)
/** One shared surface. Sent for every index before the first frame of its generation. */
typedef struct
{
	mach_msg_header_t header;
	mach_msg_body_t body;
	mach_msg_port_descriptor_t surface; /* IOSurfaceCreateMachPort() */
	uint32_t windowId;
	uint32_t generation;
	uint32_t index;
	uint32_t width; /* pixels */
	uint32_t height;
} SdlLauncherSurfaceMessage;

/** The named surface holds a complete picture. */
typedef struct
{
	mach_msg_header_t header;
	uint32_t windowId;
	uint32_t generation;
	uint32_t index;
	uint64_t sequence; /* per window, counts frames announced */
} SdlLauncherFrameMessage;
#pragma pack(pop)
