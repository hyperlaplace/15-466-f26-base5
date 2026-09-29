#pragma once

#include <glm/glm.hpp>

#include <string>
#include <list>
#include <random>

struct Connection;

//Game state, separate from rendering.
//
//"Pot Sumo": a few clay pots fight on a stone platform floating over the sea. The platform
// shrinks as the round goes on; the last pot standing wins the round.
//
//Networking model: clients send their *controls* (which buttons are held), the server owns all
// game state, runs the physics at a fixed tick, and sends the *whole* state to every client.

enum class Message : uint8_t {
	C2S_Controls = 1,
	S2C_State = 's',
};

//used to represent a control input:
struct Button {
	uint8_t downs = 0; //times the button has been pressed
	bool pressed = false; //is the button pressed now
};

//round phases (the server steps through these):
enum class Phase : uint8_t {
	Waiting = 0, //fewer than two pots: free roam, waiting for an opponent
	Countdown = 1, //pots placed on the platform, can't move yet
	Playing = 2, //fight!
	Over = 3 //round finished, winner is shown for a moment
};

//state of one player in the game:
struct Player {
	//player inputs (sent from client):
	struct Controls {
		Button left, right, up, down, jump; //'jump' is the dash button

		void send_controls_message(Connection *connection) const;

		//returns 'false' if no message or not a controls message,
		//returns 'true' if read a controls message,
		//throws on malformed controls message
		bool recv_controls_message(Connection *connection);
	} controls;

	//player state (sent from server):
	uint8_t id = 0; //unique while the player is connected
	uint8_t color_index = 0; //which pot mesh / colour this player has
	glm::vec2 position = glm::vec2(0.0f, 0.0f); //on the platform plane
	glm::vec2 velocity = glm::vec2(0.0f, 0.0f);
	float height = 0.0f; //vertical offset; goes negative once the pot has fallen off
	bool alive = true; //still in the current round
	bool falling = false; //has left the platform
	bool in_round = false; //was on the platform when the round began
	uint8_t score = 0; //rounds won
	float dash_cooldown = 0.0f; //seconds until dash is ready again
	float height_velocity = 0.0f; //(server only) vertical speed while falling
	float dash_time = 0.0f; //seconds of "heavy" impact left after a dash (server-only use, but sent for effects)

	std::string name = "";
};

struct Game {
	std::list< Player > players; //(using list so they can have stable addresses)
	Player *spawn_player(); //add player the end of the players list
	void remove_player(Player *); //remove player from game

	std::mt19937 mt; //used for spawning players
	uint32_t next_player_number = 1; //used for naming players
	uint8_t next_id = 0; //used to give players unique ids

	//round state (sent from server):
	Phase phase = Phase::Waiting;
	float phase_time = 0.0f; //time spent in the current phase
	float platform_radius = PlatformStartRadius;
	uint8_t winner_id = 255; //id of last round's winner (255 == nobody / draw)
	uint8_t round_number = 0;

	Game();

	//state update function (server only):
	void update(float elapsed);

	//constants:
	//the update rate on the server:
	inline static constexpr float Tick = 1.0f / 30.0f;

	inline static constexpr uint32_t MaxPlayers = 8;

	//platform:
	inline static constexpr float PlatformStartRadius = 4.0f;
	inline static constexpr float PlatformMinRadius = 1.4f;
	inline static constexpr float ShrinkDelay = 4.0f; //seconds before it starts shrinking
	inline static constexpr float ShrinkDuration = 40.0f; //seconds to shrink from start to min radius

	//round timing:
	inline static constexpr float CountdownTime = 3.0f;
	inline static constexpr float OverTime = 4.0f;

	//pot physics:
	inline static constexpr float PotRadius = 0.5f;
	inline static constexpr float PotSpeed = 4.2f;
	inline static constexpr float PotControlHalflife = 0.16f; //smaller = snappier
	inline static constexpr float DashSpeed = 11.0f; //speed added by a dash
	inline static constexpr float DashCooldown = 1.6f;
	inline static constexpr float DashHeavyTime = 0.45f; //a dashing pot hits harder for this long
	inline static constexpr float DashHeavyMass = 2.6f;
	inline static constexpr float Restitution = 1.15f; //>1 so that bumps feel punchy
	inline static constexpr float Gravity = 22.0f; //applied once a pot is off the platform
	inline static constexpr float KillHeight = -9.0f;

	//---- communication helpers ----

	//used by client:
	//set game state from data in connection buffer
	// (return true if data was read)
	bool recv_state_message(Connection *connection);

	//used by server:
	//send game state.
	//  Will move "connection_player" to the front of the front of the sent list.
	void send_state_message(Connection *connection, Player *connection_player = nullptr) const;

private:
	void start_round();
	void respawn(Player &player);
};
