#include "Game.hpp"

#include "Connection.hpp"

#include <stdexcept>
#include <iostream>
#include <cstring>
#include <cmath>
#include <cassert>
#include <algorithm>
#include <array>
#include <vector>
#include <utility>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/norm.hpp>

void Player::Controls::send_controls_message(Connection *connection_) const {
	assert(connection_);
	auto &connection = *connection_;

	uint32_t size = 5;
	connection.send(Message::C2S_Controls);
	connection.send(uint8_t(size));
	connection.send(uint8_t(size >> 8));
	connection.send(uint8_t(size >> 16));

	auto send_button = [&](Button const &b) {
		if (b.downs & 0x80) {
			std::cerr << "Wow, you are really good at pressing buttons!" << std::endl;
		}
		connection.send(uint8_t( (b.pressed ? 0x80 : 0x00) | (b.downs & 0x7f) ) );
	};

	send_button(left);
	send_button(right);
	send_button(up);
	send_button(down);
	send_button(jump);
}

bool Player::Controls::recv_controls_message(Connection *connection_) {
	assert(connection_);
	auto &connection = *connection_;

	auto &recv_buffer = connection.recv_buffer;

	//expecting [type, size_low0, size_mid8, size_high8]:
	if (recv_buffer.size() < 4) return false;
	if (recv_buffer[0] != uint8_t(Message::C2S_Controls)) return false;
	uint32_t size = (uint32_t(recv_buffer[3]) << 16)
	              | (uint32_t(recv_buffer[2]) << 8)
	              |  uint32_t(recv_buffer[1]);
	if (size != 5) throw std::runtime_error("Controls message with size " + std::to_string(size) + " != 5!");

	//expecting complete message:
	if (recv_buffer.size() < 4 + size) return false;

	auto recv_button = [](uint8_t byte, Button *button) {
		button->pressed = (byte & 0x80);
		uint32_t d = uint32_t(button->downs) + uint32_t(byte & 0x7f);
		if (d > 255) {
			std::cerr << "got a whole lot of downs" << std::endl;
			d = 255;
		}
		button->downs = uint8_t(d);
	};

	recv_button(recv_buffer[4+0], &left);
	recv_button(recv_buffer[4+1], &right);
	recv_button(recv_buffer[4+2], &up);
	recv_button(recv_buffer[4+3], &down);
	recv_button(recv_buffer[4+4], &jump);

	//delete message from buffer:
	recv_buffer.erase(recv_buffer.begin(), recv_buffer.begin() + 4 + size);

	return true;
}


//-----------------------------------------

Game::Game() : mt(0x15466666) {
}

//uniform random float in [0,1):
static float rand01(std::mt19937 &mt) {
	return (mt() >> 8) / float(1 << 24);
}

//a pot is "solid" (can bump and be bumped) if it's on the platform and still in play:
static bool solid(Player const &p) {
	return p.alive && !p.falling;
}

void Game::respawn(Player &player) {
	float ang = rand01(mt) * 2.0f * float(M_PI);
	float rad = std::sqrt(rand01(mt)) * 1.5f;
	player.position = rad * glm::vec2(std::cos(ang), std::sin(ang));
	player.velocity = glm::vec2(0.0f);
	player.height = 0.0f;
	player.alive = true;
	player.falling = false;
	player.dash_cooldown = 0.0f;
	player.dash_time = 0.0f;
}

Player *Game::spawn_player() {
	players.emplace_back();
	Player &player = players.back();

	//unique id (skip 255, which means "nobody"):
	auto id_in_use = [&](uint8_t id) {
		for (auto const &p : players) if (&p != &player && p.id == id) return true;
		return false;
	};
	while (next_id == 255 || id_in_use(next_id)) ++next_id;
	player.id = next_id++;

	//smallest colour not already used:
	{
		std::array< bool, MaxPlayers > used;
		used.fill(false);
		for (auto const &p : players) if (&p != &player && p.color_index < MaxPlayers) used[p.color_index] = true;
		player.color_index = 0;
		while (player.color_index + 1 < MaxPlayers && used[player.color_index]) ++player.color_index;
	}

	player.name = "Pot " + std::to_string(next_player_number++);

	respawn(player);

	if (phase == Phase::Playing || phase == Phase::Over) {
		//round in progress: watch this one, then join in when the next one starts
		player.alive = false;
		player.in_round = false;
		player.height = KillHeight;
	} else {
		player.in_round = true;
		if (phase == Phase::Countdown) {
			//re-deal everybody's starting spots so the newcomer doesn't overlap someone:
			start_round();
			phase_time = 0.0f;
		}
	}

	return &player;
}

void Game::remove_player(Player *player) {
	bool found = false;
	for (auto pi = players.begin(); pi != players.end(); ++pi) {
		if (&*pi == player) {
			players.erase(pi);
			found = true;
			break;
		}
	}
	assert(found);
}

void Game::start_round() {
	phase = Phase::Countdown;
	phase_time = 0.0f;
	platform_radius = PlatformStartRadius;
	winner_id = 255;
	round_number += 1;

	//deal everybody out around a circle, facing the middle:
	uint32_t n = uint32_t(players.size());
	uint32_t i = 0;
	for (auto &p : players) {
		float ang = (i + 0.5f) / float(n) * 2.0f * float(M_PI);
		float rad = std::min(2.4f, 0.6f * PlatformStartRadius);
		p.position = rad * glm::vec2(std::cos(ang), std::sin(ang));
		p.velocity = glm::vec2(0.0f);
		p.height = 0.0f;
		p.alive = true;
		p.falling = false;
		p.in_round = true;
		p.dash_cooldown = 0.0f;
		p.dash_time = 0.0f;
		++i;
	}
}

void Game::update(float elapsed) {
	phase_time += elapsed;

	//----- input handling (once per tick) -----
	bool can_move = (phase == Phase::Waiting || phase == Phase::Playing || phase == Phase::Over);
	std::vector< std::pair< Player *, glm::vec2 > > wish; //desired velocity for each pot
	for (auto &p : players) {
		glm::vec2 dir = glm::vec2(0.0f);
		if (p.controls.left.pressed) dir.x -= 1.0f;
		if (p.controls.right.pressed) dir.x += 1.0f;
		if (p.controls.down.pressed) dir.y -= 1.0f;
		if (p.controls.up.pressed) dir.y += 1.0f;
		if (dir != glm::vec2(0.0f)) dir = glm::normalize(dir);

		if (p.dash_cooldown > 0.0f) p.dash_cooldown = std::max(0.0f, p.dash_cooldown - elapsed);
		if (p.dash_time > 0.0f) p.dash_time = std::max(0.0f, p.dash_time - elapsed);

		if (can_move && solid(p) && p.controls.jump.downs > 0 && p.dash_cooldown <= 0.0f) {
			//dash along the input direction (or the way we're already going):
			glm::vec2 d = dir;
			if (d == glm::vec2(0.0f) && glm::length2(p.velocity) > 1e-4f) d = glm::normalize(p.velocity);
			if (d != glm::vec2(0.0f)) {
				p.velocity += d * DashSpeed;
				p.dash_cooldown = DashCooldown;
				p.dash_time = DashHeavyTime;
			}
		}

		p.controls.left.downs = 0;
		p.controls.right.downs = 0;
		p.controls.up.downs = 0;
		p.controls.down.downs = 0;
		p.controls.jump.downs = 0;

		wish.emplace_back(&p, (can_move && solid(p)) ? dir * PotSpeed : glm::vec2(0.0f));
	}

	//----- shrink the platform -----
	if (phase == Phase::Playing) {
		float t = (phase_time - ShrinkDelay) / ShrinkDuration;
		t = glm::clamp(t, 0.0f, 1.0f);
		platform_radius = glm::mix(PlatformStartRadius, PlatformMinRadius, t);
	}

	//----- physics, in a few substeps so fast dashes can't tunnel through each other -----
	const int SubSteps = 3;
	const float dt = elapsed / SubSteps;
	for (int step = 0; step < SubSteps; ++step) {
		for (auto &[pp, target] : wish) {
			Player &p = *pp;
			if (p.falling) {
				p.velocity *= std::pow(0.5f, dt / 1.5f); //slight air drag
				p.position += p.velocity * dt;
				p.height_velocity -= Gravity * dt;
				p.height += p.height_velocity * dt;
				continue;
			}
			if (!p.alive) continue; //spectators don't simulate

			//steer velocity toward the wished-for velocity (this also bleeds off dash speed):
			float amt = 1.0f - std::pow(0.5f, dt / PotControlHalflife);
			p.velocity = glm::mix(p.velocity, target, amt);
			p.position += p.velocity * dt;
		}

		//pot / pot bumping:
		for (auto &p1 : players) {
			if (!solid(p1)) continue;
			for (auto &p2 : players) {
				if (&p1 == &p2) break;
				if (!solid(p2)) continue;
				glm::vec2 d = p2.position - p1.position;
				float len2 = glm::length2(d);
				float min_dist = 2.0f * PotRadius;
				if (len2 >= min_dist * min_dist) continue;
				float len = std::sqrt(len2);
				glm::vec2 n = (len > 1e-5f) ? d / len : glm::vec2(1.0f, 0.0f);

				float m1 = (p1.dash_time > 0.0f) ? DashHeavyMass : 1.0f;
				float m2 = (p2.dash_time > 0.0f) ? DashHeavyMass : 1.0f;

				//push apart (lighter pot moves more):
				float overlap = min_dist - len;
				p1.position -= n * overlap * (m2 / (m1 + m2));
				p2.position += n * overlap * (m1 / (m1 + m2));

				//impulse along the contact normal:
				float vn = glm::dot(p2.velocity - p1.velocity, n);
				if (vn < 0.0f) {
					float j = -(1.0f + Restitution) * vn / (1.0f / m1 + 1.0f / m2);
					p1.velocity -= n * (j / m1);
					p2.velocity += n * (j / m2);
				}
			}
		}

		//edge check: once a pot's centre is past the rim, it starts to fall
		for (auto &p : players) {
			if (!solid(p)) continue;
			if (glm::length(p.position) > platform_radius) {
				p.falling = true;
				p.alive = false;
				p.height_velocity = 0.0f;
			}
		}
	}

	//----- phase logic -----
	switch (phase) {
		case Phase::Waiting: {
			for (auto &p : players) {
				if (p.falling && p.height < KillHeight) respawn(p); //no penalty while warming up
			}
			if (players.size() >= 2) start_round();
			break;
		}
		case Phase::Countdown: {
			if (players.size() < 2) {
				phase = Phase::Waiting;
				phase_time = 0.0f;
			} else if (phase_time >= CountdownTime) {
				phase = Phase::Playing;
				phase_time = 0.0f;
			}
			break;
		}
		case Phase::Playing: {
			uint32_t alive_count = 0;
			Player *last = nullptr;
			for (auto &p : players) {
				if (p.in_round && p.alive) { ++alive_count; last = &p; }
			}
			if (alive_count <= 1) {
				phase = Phase::Over;
				phase_time = 0.0f;
				if (alive_count == 1) {
					winner_id = last->id;
					last->score = uint8_t(std::min< int >(255, last->score + 1));
				} else {
					winner_id = 255;
				}
			}
			break;
		}
		case Phase::Over: {
			if (phase_time >= OverTime) {
				if (players.size() >= 2) {
					start_round();
				} else {
					phase = Phase::Waiting;
					phase_time = 0.0f;
					winner_id = 255;
					platform_radius = PlatformStartRadius;
					for (auto &p : players) { respawn(p); p.in_round = true; }
				}
			}
			break;
		}
	}
}


void Game::send_state_message(Connection *connection_, Player *connection_player) const {
	assert(connection_);
	auto &connection = *connection_;

	connection.send(Message::S2C_State);
	//will patch message size in later, for now placeholder bytes:
	connection.send(uint8_t(0));
	connection.send(uint8_t(0));
	connection.send(uint8_t(0));
	size_t mark = connection.send_buffer.size(); //keep track of this position in the buffer

	//round info:
	connection.send(uint8_t(phase));
	connection.send(phase_time);
	connection.send(platform_radius);
	connection.send(winner_id);
	connection.send(round_number);

	//send player info helper:
	auto send_player = [&](Player const &player) {
		connection.send(player.id);
		connection.send(player.color_index);
		connection.send(player.position);
		connection.send(player.velocity);
		connection.send(player.height);
		connection.send(uint8_t( (player.alive ? 1 : 0) | (player.falling ? 2 : 0) | (player.in_round ? 4 : 0) ));
		connection.send(player.score);
		connection.send(player.dash_cooldown);
		connection.send(player.dash_time);

		//NOTE: can't just 'send(name)' because player.name is not plain-old-data type.
		//effectively: truncates player name to 255 chars
		uint8_t len = uint8_t(std::min< size_t >(255, player.name.size()));
		connection.send(len);
		connection.send_buffer.insert(connection.send_buffer.end(), player.name.begin(), player.name.begin() + len);
	};

	//player count:
	connection.send(uint8_t(players.size()));
	if (connection_player) send_player(*connection_player);
	for (auto const &player : players) {
		if (&player == connection_player) continue;
		send_player(player);
	}

	//compute the message size and patch into the message header:
	uint32_t size = uint32_t(connection.send_buffer.size() - mark);
	connection.send_buffer[mark-3] = uint8_t(size);
	connection.send_buffer[mark-2] = uint8_t(size >> 8);
	connection.send_buffer[mark-1] = uint8_t(size >> 16);
}

bool Game::recv_state_message(Connection *connection_) {
	assert(connection_);
	auto &connection = *connection_;
	auto &recv_buffer = connection.recv_buffer;

	if (recv_buffer.size() < 4) return false;
	if (recv_buffer[0] != uint8_t(Message::S2C_State)) return false;
	uint32_t size = (uint32_t(recv_buffer[3]) << 16)
	              | (uint32_t(recv_buffer[2]) << 8)
	              |  uint32_t(recv_buffer[1]);
	uint32_t at = 0;
	//expecting complete message:
	if (recv_buffer.size() < 4 + size) return false;

	//copy bytes from buffer and advance position:
	auto read = [&](auto *val) {
		if (at + sizeof(*val) > size) {
			throw std::runtime_error("Ran out of bytes reading state message.");
		}
		std::memcpy(val, &recv_buffer[4 + at], sizeof(*val));
		at += sizeof(*val);
	};

	uint8_t phase_byte;
	read(&phase_byte);
	if (phase_byte > uint8_t(Phase::Over)) throw std::runtime_error("Unknown phase in state message.");
	phase = Phase(phase_byte);
	read(&phase_time);
	read(&platform_radius);
	read(&winner_id);
	read(&round_number);

	players.clear();
	uint8_t player_count;
	read(&player_count);
	for (uint8_t i = 0; i < player_count; ++i) {
		players.emplace_back();
		Player &player = players.back();
		read(&player.id);
		read(&player.color_index);
		read(&player.position);
		read(&player.velocity);
		read(&player.height);
		uint8_t flags;
		read(&flags);
		player.alive = (flags & 1);
		player.falling = (flags & 2);
		player.in_round = (flags & 4);
		read(&player.score);
		read(&player.dash_cooldown);
		read(&player.dash_time);
		uint8_t name_len;
		read(&name_len);
		//n.b. would probably be more efficient to directly copy from recv_buffer, but I think this is clearer:
		player.name = "";
		for (uint8_t n = 0; n < name_len; ++n) {
			char c;
			read(&c);
			player.name += c;
		}
	}

	if (at != size) throw std::runtime_error("Trailing data in state message.");

	//delete message from buffer:
	recv_buffer.erase(recv_buffer.begin(), recv_buffer.begin() + 4 + size);

	return true;
}
