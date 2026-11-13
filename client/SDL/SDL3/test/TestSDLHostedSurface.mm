/** Hosted rendering output: SDL draws into shared surfaces and a launcher-side port receives
 * them. Needs a Metal renderer, so it uses the real video driver with a window that stays hidden. */
#include "../sdl_hosted_surface.hpp"
#include "../sdl_launcher_surface.h"

#import <IOSurface/IOSurface.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>

static void require(bool value, const char* message)
{
	if (!value)
		throw std::runtime_error(message);
}

union Received {
	mach_msg_header_t header;
	SdlLauncherSurfaceMessage surface;
	SdlLauncherFrameMessage frame;
	uint8_t bytes[256];
};

static bool receive(mach_port_t port, Received& message, unsigned timeoutMs = 2000)
{
	memset(&message, 0, sizeof(message));
	return mach_msg(&message.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(message), port,
	                timeoutMs, MACH_PORT_NULL) == KERN_SUCCESS;
}

/* The centre pixel of a surface as B, G, R, waiting for the GPU to finish drawing it. */
static bool waitForPixel(IOSurfaceRef surface, uint8_t b, uint8_t g, uint8_t r)
{
	for (int attempt = 0; attempt < 200; attempt++)
	{
		IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr);
		const auto base = static_cast<const uint8_t*>(IOSurfaceGetBaseAddress(surface));
		const auto pixel = base + (IOSurfaceGetHeight(surface) / 2) * IOSurfaceGetBytesPerRow(surface) +
		                   (IOSurfaceGetWidth(surface) / 2) * 4;
		const bool match = pixel[0] == b && pixel[1] == g && pixel[2] == r;
		IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
		if (match)
			return true;
		SDL_Delay(5);
	}
	return false;
}

static bool draw(SDL_Window* window, SDL_Renderer* renderer, Uint8 r, Uint8 g, Uint8 b)
{
	return SdlHosted::screenTarget(window, renderer) &&
	       SDL_SetRenderDrawColor(renderer, r, g, b, 0xff) && SDL_RenderClear(renderer) &&
	       SdlHosted::present(window, renderer);
}

int main()
{
	try
	{
		SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
		require(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
		auto window = SDL_CreateWindow("Sheltie hosted surface test", 320, 200,
		                               SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
		require(window != nullptr, SDL_GetError());
		auto renderer = SDL_CreateRenderer(window, nullptr);
		require(renderer != nullptr, SDL_GetError());
		if (strcmp(SDL_GetRendererName(renderer), "metal") != 0)
		{
			printf("SKIPPED: renderer is %s, not metal\n", SDL_GetRendererName(renderer));
			return 0;
		}
		const auto id = SDL_GetWindowID(window);

		mach_port_t port = MACH_PORT_NULL;
		require(mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) == KERN_SUCCESS,
		        "cannot allocate a port");
		require(mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND) ==
		            KERN_SUCCESS,
		        "cannot make a send right");

		auto& surfaces = SdlHostedSurfaces::instance();
		SdlHosted::enable(true);
		SdlHosted::setOutput(&surfaces);

		/* Without a port there is nowhere to draw. */
		require(!surfaces.connected(), "connected without a port");
		require(SdlHosted::setLayout(id, { 640, 400, 2.0f }), "layout refused");
		require(!SdlHosted::screenTarget(window, renderer), "drew without a port");

		surfaces.setPort(port);
		require(surfaces.connected(), "not connected after a port was set");

		/* First frame: three surfaces of the layout's pixel size, then the frame that names one. */
		require(draw(window, renderer, 0xff, 0x00, 0x00), "first frame failed");
		IOSurfaceRef shared[SDL_LAUNCHER_SURFACE_COUNT] = {};
		Received message;
		for (uint32_t i = 0; i < SDL_LAUNCHER_SURFACE_COUNT; i++)
		{
			require(receive(port, message), "surface message missing");
			require(message.header.msgh_id == SDL_LAUNCHER_MSG_SURFACE, "expected a surface message");
			require(message.surface.windowId == id && message.surface.generation == 1 &&
			            message.surface.index == i,
			        "surface message names the wrong window, generation or index");
			require(message.surface.width == 640 && message.surface.height == 400,
			        "surface is not the layout's pixel size");
			shared[i] = IOSurfaceLookupFromMachPort(message.surface.surface.name);
			mach_port_deallocate(mach_task_self(), message.surface.surface.name);
			require(shared[i] != nullptr, "surface port does not name a surface");
			require(IOSurfaceGetWidth(shared[i]) == 640 && IOSurfaceGetHeight(shared[i]) == 400,
			        "received surface has the wrong size");
		}
		require(receive(port, message) && message.header.msgh_id == SDL_LAUNCHER_MSG_FRAME,
		        "frame message missing");
		require(message.frame.windowId == id && message.frame.generation == 1 &&
		            message.frame.index == 0 && message.frame.sequence == 1,
		        "first frame message is wrong");
		require(waitForPixel(shared[0], 0x00, 0x00, 0xff), "the picture is not in the surface");

		/* Later frames rotate through the surfaces and count up. */
		require(draw(window, renderer, 0x00, 0xff, 0x00), "second frame failed");
		require(receive(port, message) && message.header.msgh_id == SDL_LAUNCHER_MSG_FRAME &&
		            message.frame.index == 1 && message.frame.sequence == 2,
		        "second frame did not use the next surface");
		require(waitForPixel(shared[1], 0x00, 0xff, 0x00), "second picture is not in its surface");
		require(waitForPixel(shared[0], 0x00, 0x00, 0xff), "the previous surface was drawn over");
		require(draw(window, renderer, 0x00, 0x00, 0xff), "third frame failed");
		require(receive(port, message) && message.frame.index == 2, "third frame index wrong");
		require(draw(window, renderer, 0x10, 0x20, 0x30), "fourth frame failed");
		require(receive(port, message) && message.frame.index == 0 && message.frame.sequence == 4,
		        "rotation did not wrap");

		/* A new layout makes a new generation of surfaces in the new size. */
		require(SdlHosted::setLayout(id, { 800, 500, 2.0f }), "second layout refused");
		require(draw(window, renderer, 0xff, 0xff, 0x00), "frame after resize failed");
		IOSurfaceRef resized = nullptr;
		for (uint32_t i = 0; i < SDL_LAUNCHER_SURFACE_COUNT; i++)
		{
			require(receive(port, message) && message.header.msgh_id == SDL_LAUNCHER_MSG_SURFACE,
			        "surface message missing after resize");
			require(message.surface.generation == 2 && message.surface.index == i &&
			            message.surface.width == 800 && message.surface.height == 500,
			        "resized surface message is wrong");
			if (i == 0)
				resized = IOSurfaceLookupFromMachPort(message.surface.surface.name);
			mach_port_deallocate(mach_task_self(), message.surface.surface.name);
		}
		require(receive(port, message) && message.header.msgh_id == SDL_LAUNCHER_MSG_FRAME &&
		            message.frame.generation == 2 && message.frame.index == 0,
		        "frame after resize is wrong");
		require(resized && waitForPixel(resized, 0x00, 0xff, 0xff), "resized picture missing");

		/* Announced means complete. Each frame is a heavy draw ending in one flat colour; the
		 * surface is read the moment the frame message is sent. With the wait the picture must
		 * always be there. Without it the count shows what the wait prevents on this machine. */
		{
			auto heavy = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_BGRA32, SDL_TEXTUREACCESS_TARGET, 2048, 2048);
			require(heavy != nullptr, SDL_GetError());
			IOSurfaceRef current[SDL_LAUNCHER_SURFACE_COUNT] = {};
			require(SdlHosted::setLayout(id, { 2560, 1440, 2.0f }), "large layout refused");
			for (int pass = 0; pass < 2; pass++)
			{
				const bool wait = pass == 1;
				surfaces.setWaitForGpu(wait);
				int incomplete = 0;
				Uint64 presentNs = 0;
				const int frames = 120;
				for (int frame = 0; frame < frames; frame++)
				{
					const Uint8 shade = static_cast<Uint8>(40 + (frame % 2) * 160 + pass * 20);
					require(SdlHosted::screenTarget(window, renderer), "target failed");
					for (int layer = 0; layer < 40; layer++)
						require(SDL_RenderTexture(renderer, heavy, nullptr, nullptr), SDL_GetError());
					require(SDL_SetRenderDrawColor(renderer, shade, shade, shade, 0xff) &&
					            SDL_RenderClear(renderer),
					        SDL_GetError());
					for (int layer = 0; layer < 40; layer++)
					{
						const SDL_FRect whole = { 0, 0, 2560, 1440 };
						require(SDL_RenderFillRect(renderer, &whole), SDL_GetError());
					}
					const auto before = SDL_GetTicksNS();
					require(SdlHosted::present(window, renderer), "present failed");
					presentNs += SDL_GetTicksNS() - before;
					uint32_t index = UINT32_MAX;
					while (receive(port, message, 0))
					{
						if (message.header.msgh_id == SDL_LAUNCHER_MSG_SURFACE)
						{
							if (current[message.surface.index])
								CFRelease(current[message.surface.index]);
							current[message.surface.index] = IOSurfaceLookupFromMachPort(message.surface.surface.name);
							mach_port_deallocate(mach_task_self(), message.surface.surface.name);
						}
						else
							index = message.frame.index;
					}
					require(index < SDL_LAUNCHER_SURFACE_COUNT && current[index], "frame message missing in stress run");
					IOSurfaceLock(current[index], kIOSurfaceLockReadOnly, nullptr);
					const auto base = static_cast<const uint8_t*>(IOSurfaceGetBaseAddress(current[index]));
					const auto row = IOSurfaceGetBytesPerRow(current[index]);
					bool complete = true;
					for (size_t y : { size_t(0), size_t(720), size_t(1439) })
						for (size_t x : { size_t(0), size_t(1280), size_t(2559) })
							complete = complete && base[y * row + x * 4] == shade;
					IOSurfaceUnlock(current[index], kIOSurfaceLockReadOnly, nullptr);
					if (!complete)
						incomplete++;
				}
				printf("wait for GPU %s: %d of %d frames announced before they were complete, present %.2f ms average\n",
				       wait ? "on" : "off", incomplete, frames, static_cast<double>(presentNs) / frames / 1e6);
				require(!wait || incomplete == 0, "a frame was announced before the GPU finished it");
			}
			for (auto surface : current)
				if (surface)
					CFRelease(surface);
			SDL_DestroyTexture(heavy);
			require(SdlHosted::setLayout(id, { 800, 500, 2.0f }), "layout refused after stress run");
			require(draw(window, renderer, 0xff, 0xff, 0x00), "frame after stress run failed");
			while (receive(port, message, 0))
			{
				if (message.header.msgh_id == SDL_LAUNCHER_MSG_SURFACE)
					mach_port_deallocate(mach_task_self(), message.surface.surface.name);
			}
		}

		/* A full queue drops frame messages without failing or blocking. */
		const auto before = SDL_GetTicks();
		for (int i = 0; i < 40; i++)
			require(draw(window, renderer, 0x40, 0x40, 0x40), "frame failed with a full queue");
		require(SDL_GetTicks() - before < 2000, "a full queue blocked the render loop");
		int queued = 0;
		while (receive(port, message, 0))
			queued++;
		require(queued > 0 && queued < 40, "frame messages were neither queued nor dropped");

		/* Forgetting a window frees its surfaces; a later frame starts the next generation. */
		SdlHosted::forget(id);
		require(SdlHosted::setLayout(id, { 800, 500, 2.0f }), "layout refused after forget");
		require(draw(window, renderer, 0x00, 0x00, 0x00), "frame after forget failed");
		require(receive(port, message) && message.header.msgh_id == SDL_LAUNCHER_MSG_SURFACE &&
		            message.surface.generation == 5,
		        "generation was reused after forget");
		mach_port_deallocate(mach_task_self(), message.surface.surface.name);
		while (receive(port, message, 0))
		{
			if (message.header.msgh_id == SDL_LAUNCHER_MSG_SURFACE)
				mach_port_deallocate(mach_task_self(), message.surface.surface.name);
		}

		/* The launcher going away is noticed. */
		require(mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1) == KERN_SUCCESS,
		        "cannot destroy the receive right");
		require(!draw(window, renderer, 0xff, 0xff, 0xff), "drew to a dead port");
		require(surfaces.dead() && !surfaces.connected(), "dead port not noticed");

		for (auto surface : shared)
			CFRelease(surface);
		CFRelease(resized);
		SdlHosted::forget(id);
		SdlHosted::setOutput(nullptr);
		SdlHosted::enable(false);
		SDL_DestroyRenderer(renderer);
		SDL_DestroyWindow(window);
		SDL_Quit();
		printf("Hosted surfaces: ok\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		fprintf(stderr, "TestSDLHostedSurface failed: %s\n", error.what());
		return 1;
	}
}
