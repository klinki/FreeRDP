/** Hosted rendering output: shared IOSurfaces the SDL renderer draws into, announced to the
 * launcher over Mach (launcher-bridge.md, "Surface channel"). SDL main thread only. */
#pragma once
#include <cstdint>
#include <map>
#include "sdl_hosted.hpp"

class SdlHostedSurfaces : public SdlHosted::Output
{
  public:
	static SdlHostedSurfaces& instance();

	/** Takes the launcher's port from the registered-port slot. False when there is none. */
	bool adoptInheritedPort();
	/** Uses the given send right instead; the caller keeps its own reference. For tests. */
	void setPort(uint32_t sendRight);
	bool connected() const { return _port != 0 && !_dead; }
	/** The launcher's port went away: the session should end. */
	bool dead() const { return _dead; }
	/** Whether present() waits for the GPU to finish the picture before announcing it. On by
	 * default; switching it off is for measuring what the wait prevents and costs. */
	void setWaitForGpu(bool wait) { _waitForGpu = wait; }

	SDL_Texture* target(SDL_Window* window, SDL_Renderer* renderer) override;
	bool present(SDL_Window* window, SDL_Renderer* renderer) override;
	void forget(SDL_WindowID id) override;

  private:
	struct Set
	{
		void* surface[3] = {}; // IOSurfaceRef
		SDL_Texture* texture[3] = {};
		int width = 0;
		int height = 0;
		uint32_t generation = 0;
		uint32_t current = 0;
		uint64_t sequence = 0;
	};
	void release(Set& set);
	bool create(Set& set, SDL_WindowID id, SDL_Renderer* renderer, int width, int height);
	bool send(void* header, bool wait);

	std::map<SDL_WindowID, Set> _sets;
	std::map<SDL_WindowID, uint32_t> _generations; // survive forget(): never reuse a generation
	uint32_t _port = 0;
	bool _dead = false;
	bool _waitForGpu = true;
};
