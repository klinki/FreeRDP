// Test host for hosted rendering (client/SDL/SDL3/launcher-bridge.md, "Hosted rendering").
// Stands in for the launcher's window side: it starts the engine with the surface port in the
// inherited registered-port slot, then shows every window's surfaces in an AppKit window.
//
// Usage: HOSTED_ENGINE=/path/to/sdl-freerdp hosted-host <engine arguments...>
// All arguments and inherited file descriptors go to the engine unchanged, so a bridge driver can
// start this program in place of the engine. HOSTED_SCALE sets the backing scale assumed for the
// windows (default 2). Prints one line per window and one statistics line per second.
//
// Build: clang -fobjc-arc -O1 -I client/SDL/SDL3 -o hosted-host scripts/sheltie/hosted-host.m \
//          -framework Cocoa -framework QuartzCore -framework IOSurface
#import <Cocoa/Cocoa.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/QuartzCore.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <sys/wait.h>
#include "sdl_launcher_surface.h"

extern char** environ;

@interface HostedWindow : NSObject
@property NSWindow* window;
@property CALayer* content;
@property uint32_t generation;
@property NSMutableDictionary<NSNumber*, id>* surfaces; // index -> IOSurface
@end
@implementation HostedWindow
@end

static NSMutableDictionary<NSNumber*, HostedWindow*>* windows; // receiving thread only
static _Atomic uint64_t shown, stale, lastSequence, gaps;
static pid_t engine;
static double scale = 2;

static void forwardSignal(int number)
{
	if (engine > 0)
		kill(engine, number);
	_exit(0);
}

static HostedWindow* windowFor(uint32_t windowId, uint32_t width, uint32_t height)
{
	HostedWindow* hosted = windows[@(windowId)];
	const NSSize size = NSMakeSize(width / scale, height / scale);
	if (!hosted)
	{
		hosted = [HostedWindow new];
		hosted.surfaces = [NSMutableDictionary new];
		windows[@(windowId)] = hosted;
		const NSUInteger count = windows.count;
		dispatch_sync(dispatch_get_main_queue(), ^{
			NSWindow* window = [[NSWindow alloc]
			    initWithContentRect:NSMakeRect(80 + 40 * count, 120, size.width, size.height)
			              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
			                        NSWindowStyleMaskResizable
			                backing:NSBackingStoreBuffered
			                  defer:NO];
			window.title = [NSString stringWithFormat:@"Hosted rendering test host, window %u", windowId];
			window.releasedWhenClosed = NO;
			NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, size.width, size.height)];
			view.wantsLayer = YES;
			view.layer.backgroundColor = NSColor.systemGreenColor.CGColor;
			window.contentView = view;
			CALayer* content = [CALayer layer];
			content.frame = view.layer.bounds;
			content.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
			content.contentsGravity = kCAGravityResize;
			[view.layer addSublayer:content];
			[window orderFrontRegardless];
			hosted.window = window;
			hosted.content = content;
			printf("windowNumber=%ld windowId=%u\n", (long)window.windowNumber, windowId);
		});
	}
	return hosted;
}

static void receiveLoop(mach_port_t port)
{
	windows = [NSMutableDictionary new];
	for (;;)
	{
		union {
			mach_msg_header_t header;
			SdlLauncherSurfaceMessage surface;
			SdlLauncherFrameMessage frame;
			uint8_t bytes[512];
		} message;
		if (mach_msg(&message.header, MACH_RCV_MSG, 0, sizeof(message), port, MACH_MSG_TIMEOUT_NONE,
		             MACH_PORT_NULL) != KERN_SUCCESS)
			continue;
		@autoreleasepool
		{
			if (message.header.msgh_id == SDL_LAUNCHER_MSG_SURFACE)
			{
				const SdlLauncherSurfaceMessage* m = &message.surface;
				IOSurfaceRef surface = IOSurfaceLookupFromMachPort(m->surface.name);
				mach_port_deallocate(mach_task_self(), m->surface.name);
				printf("surface window=%u generation=%u index=%u %ux%u %s\n", m->windowId, m->generation,
				       m->index, m->width, m->height, surface ? "ok" : "LOOKUP FAILED");
				if (!surface)
					continue;
				HostedWindow* hosted = windowFor(m->windowId, m->width, m->height);
				if (hosted.generation != m->generation)
				{
					/* Keep showing the old picture until a frame of the new generation arrives. */
					hosted.generation = m->generation;
					hosted.surfaces = [NSMutableDictionary new];
					const NSSize size = NSMakeSize(m->width / scale, m->height / scale);
					NSWindow* window = hosted.window;
					dispatch_async(dispatch_get_main_queue(), ^{
						[window setContentSize:size];
					});
				}
				hosted.surfaces[@(m->index)] = CFBridgingRelease(surface);
			}
			else if (message.header.msgh_id == SDL_LAUNCHER_MSG_FRAME)
			{
				const SdlLauncherFrameMessage* m = &message.frame;
				HostedWindow* hosted = windows[@(m->windowId)];
				id surface = hosted && hosted.generation == m->generation ? hosted.surfaces[@(m->index)] : nil;
				if (!surface)
				{
					stale++;
					continue;
				}
				/* On this thread, in its own transaction: a busy main thread cannot hold frames back. */
				[CATransaction begin];
				[CATransaction setDisableActions:YES];
				hosted.content.contents = surface;
				[CATransaction commit];
				[CATransaction flush];
				if (lastSequence && m->sequence > lastSequence + 1)
					gaps += m->sequence - lastSequence - 1;
				lastSequence = m->sequence;
				shown++;
			}
		}
	}
}

int main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	const char* path = getenv("HOSTED_ENGINE");
	if (!path)
	{
		fprintf(stderr, "HOSTED_ENGINE is not set\n");
		return 2;
	}
	if (getenv("HOSTED_SCALE"))
		scale = atof(getenv("HOSTED_SCALE"));

	mach_port_t port = MACH_PORT_NULL;
	mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
	mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
	mach_port_limits_t limits = { .mpl_qlimit = MACH_PORT_QLIMIT_LARGE };
	mach_port_set_attributes(mach_task_self(), port, MACH_PORT_LIMITS_INFO, (mach_port_info_t)&limits,
	                         MACH_PORT_LIMITS_INFO_COUNT);

	/* The engine inherits whatever is registered while it is started; clear the slot afterwards
	 * so that no later child gets this session's port. HOSTED_NO_PORT leaves it empty on purpose. */
	mach_port_t slots[1] = { getenv("HOSTED_NO_PORT") ? MACH_PORT_NULL : port };
	kern_return_t kr = mach_ports_register(mach_task_self(), slots, 1);
	argv[0] = (char*)path;
	const int rc = posix_spawn(&engine, path, NULL, NULL, argv, environ);
	slots[0] = MACH_PORT_NULL;
	mach_ports_register(mach_task_self(), slots, 1);
	printf("register=%s spawn=%s engine pid=%d\n", mach_error_string(kr), rc ? strerror(rc) : "ok", engine);
	if (rc != 0)
		return 2;
	signal(SIGTERM, forwardSignal);
	signal(SIGINT, forwardSignal);

	@autoreleasepool
	{
		NSApplication* app = [NSApplication sharedApplication];
		[app setActivationPolicy:NSApplicationActivationPolicyRegular];
		[NSThread detachNewThreadWithBlock:^{
			receiveLoop(port);
		}];
		[NSThread detachNewThreadWithBlock:^{
			int status = 0;
			waitpid(engine, &status, 0);
			printf("engine exited status=%d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status));
			exit(0);
		}];
		[NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer* timer) {
			printf("shown=%llu stale=%llu gaps=%llu\n", atomic_exchange(&shown, 0), atomic_exchange(&stale, 0),
			       atomic_exchange(&gaps, 0));
		}];
		[app run];
	}
	return 0;
}
