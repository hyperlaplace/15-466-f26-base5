#include "Mode.hpp"

#include "Connection.hpp"
#include "Game.hpp"
#include "Scene.hpp"

#include <glm/glm.hpp>

#include <vector>
#include <array>

struct PlayMode : Mode {
	PlayMode(Client &client);
	virtual ~PlayMode();

	//functions called by main loop:
	virtual bool handle_event(SDL_Event const &, glm::uvec2 const &window_size) override;
	virtual void update(float elapsed) override;
	virtual void draw(glm::uvec2 const &drawable_size) override;

	//----- game state -----

	//input tracking for local player:
	Player::Controls controls;

	//latest game state (from server):
	Game game;

	//connection to server:
	Client &client;

	//----- scene + smoothing (client-side only) -----

	Scene scene;
	Scene::Camera *camera = nullptr;
	Scene::Transform *platform_transform = nullptr;
	Scene::Transform *marker_transform = nullptr; //ring drawn under the local player's pot
	Scene::Drawable *marker_drawable = nullptr;

	//The server only sends snapshots 30 times a second, so each pot on screen eases toward the most
	// recent snapshot (extrapolating with the reported velocity in between):
	struct Visual {
		Scene::Transform *transform = nullptr;
		Scene::Drawable *drawable = nullptr;
		bool active = false;
		uint8_t id = 0;
		glm::vec2 pos = glm::vec2(0.0f); //what we draw
		glm::vec2 target = glm::vec2(0.0f); //where the server says it is (extrapolated)
		glm::vec2 vel = glm::vec2(0.0f);
		float height = 0.0f;
		float facing = 0.0f; //yaw, radians
		float fall_time = 0.0f;
		bool splashed = false;
	};
	std::array< Visual, Game::MaxPlayers > visuals;

	struct Splash {
		Scene::Transform *transform = nullptr;
		Scene::Drawable *drawable = nullptr;
		float age = -1.0f; //< 0 == inactive
	};
	std::array< Splash, 6 > splashes;

	float platform_radius_shown = Game::PlatformStartRadius;
	float shake = 0.0f; //camera shake amount
	float time = 0.0f;
	bool got_state = false;
	Phase last_phase = Phase::Waiting;
};
