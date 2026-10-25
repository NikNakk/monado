// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <array>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

// Requested placements relative to the head at the start of each trial. These
// are instructions, never ground truth or measurements for a calibration fit.
struct controller_survey_pose
{
	const char *name;
	const char *instruction;
	float outward, up, forward;
};

inline constexpr std::array<controller_survey_pose, 14> controller_survey_plan{{
    {"front", "HOLD IN FRONT", .20f, -.25f, .45f},
    {"chest", "HOLD AT CHEST HEIGHT", .18f, -.40f, .25f},
    {"low", "LOWER HAND COMFORTABLY", .20f, -.65f, .35f},
    {"high", "RAISE HAND TO EYE HEIGHT", .20f, .05f, .40f},
    {"far", "REACH FORWARD COMFORTABLY", .18f, -.20f, .60f},
    {"side", "MOVE HAND OUT TO THE SIDE", .50f, -.25f, .10f},
    {"cross_body", "REACH ACROSS YOUR CHEST", -.25f, -.30f, .30f},
    {"near_face", "HOLD BESIDE YOUR CHEEK", .22f, -.10f, .20f},
    {"palm_up", "TURN PALM UP AND HOLD", .20f, -.25f, .40f},
    {"palm_down", "TURN PALM DOWN AND HOLD", .20f, -.25f, .40f},
    {"wrist_roll", "ROLL WRIST SLOWLY", .20f, -.25f, .40f},
    {"shoulder", "HOLD BESIDE YOUR SHOULDER", .25f, -.15f, -.05f},
    {"behind_shoulder", "REACH JUST BEHIND SHOULDER", .25f, -.15f, -.25f},
    {"head_turn", "HOLD HAND AND TURN HEAD", .20f, -.25f, .45f},
}};

enum class controller_survey_phase
{
	ready,
	move,
	hold,
	recover,
	complete
};

inline const char *
controller_survey_phase_name(controller_survey_phase phase)
{
	switch (phase) {
	case controller_survey_phase::ready: return "ready";
	case controller_survey_phase::move: return "move";
	case controller_survey_phase::hold: return "hold";
	case controller_survey_phase::recover: return "recover";
	case controller_survey_phase::complete: return "complete";
	}
	return "unknown";
}

struct controller_survey_state
{
	size_t trial = 0;
	size_t end_trial = 2 * controller_survey_plan.size();
	controller_survey_phase phase = controller_survey_phase::ready;
	double elapsed = 0;
	int64_t last_ns = 0;
	bool paused = false;
	bool trigger_down = false, pause_down = false, skip_down = false;

	size_t
	hand() const
	{
		return trial / controller_survey_plan.size();
	}
	size_t
	pose_index() const
	{
		return trial % controller_survey_plan.size();
	}
	double
	duration() const
	{
		return phase == controller_survey_phase::move ? 3.0 : 5.0;
	}

	// Timed phases never wait for controller position: otherwise failures would
	// be excluded from the very dataset intended to explain them.
	const char *
	update(int64_t now, bool usable, bool trigger, bool pause, bool skip, bool allow_start = true)
	{
		const bool start_edge = trigger && !trigger_down;
		const bool pause_edge = pause && !pause_down;
		const bool skip_edge = skip && !skip_down;
		trigger_down = trigger;
		pause_down = pause;
		skip_down = skip;
		double dt = last_ns ? (now - last_ns) * 1e-9 : 0;
		last_ns = now;
		if (!usable || phase == controller_survey_phase::complete)
			return nullptr;
		if (pause_edge) {
			paused = !paused;
			return paused ? "paused" : "resumed";
		}
		if (skip_edge) {
			next();
			return "skipped";
		}
		if (paused)
			return nullptr;
		if (phase == controller_survey_phase::ready) {
			if (!start_edge || !allow_start)
				return nullptr;
			phase = controller_survey_phase::move;
			elapsed = 0;
			return "started";
		}
		// Do not consume an entire phase when rendering/focus was suspended.
		if (dt > 0 && dt <= .5)
			elapsed += dt;
		if (elapsed < duration())
			return nullptr;
		elapsed = 0;
		if (phase == controller_survey_phase::move)
			phase = controller_survey_phase::hold;
		else if (phase == controller_survey_phase::hold)
			phase = controller_survey_phase::recover;
		else
			next();
		return "phase";
	}

	void
	reset_reference()
	{
		if (phase == controller_survey_phase::complete)
			return;
		phase = controller_survey_phase::ready;
		elapsed = 0;
		paused = false;
	}

	void
	next()
	{
		++trial;
		phase = trial >= end_trial ? controller_survey_phase::complete : controller_survey_phase::ready;
		elapsed = 0;
		paused = false;
	}
};

struct controller_survey_capture
{
	std::string directory;
	controller_survey_state state;
	FILE *samples = nullptr, *events = nullptr;
	bool io_failed = false;
	std::array<std::array<uint64_t, 4>, 2 * controller_survey_plan.size()>
	    counts{}; // frames, active, position valid, position tracked during hold
	~controller_survey_capture()
	{
		close();
	}

	void
	open(const char *path, const char *hands = "both", bool passthrough = false)
	{
		if (std::string(hands) != "both" && std::string(hands) != "left" && std::string(hands) != "right")
			throw std::runtime_error("survey hand must be both, left or right");
		state.trial = std::string(hands) == "right" ? controller_survey_plan.size() : 0;
		state.end_trial =
		    std::string(hands) == "left" ? controller_survey_plan.size() : 2 * controller_survey_plan.size();
		if (samples || events)
			throw std::runtime_error("survey capture is already open");
		if (!std::filesystem::create_directory(path))
			throw std::runtime_error("survey output directory already exists; choose a new path");
		directory = path;
		samples = fopen((directory + "/poses.csv").c_str(), "w");
		events = fopen((directory + "/events.csv").c_str(), "w");
		if (!samples || !events)
			throw std::runtime_error("cannot open survey output files");
		fprintf(events, "host_uptime_ns,xr_time,trial,hand,pose,phase,event\n");
		fprintf(samples,
		        "host_uptime_ns,xr_time,trial,phase,paused,usable,hand,kind,active,result,flags,velocity_flags,"
		        "px,py,pz,qx,qy,qz,qw,vx,vy,vz,wx,wy,wz,head_flags,hx,hy,hz,hqx,hqy,hqz,hqw,"
		        "target_x,target_y,target_z,trigger,primary,secondary,look_x,look_y,look_z,look_error_deg\n");
		FILE *plan = fopen((directory + "/plan.csv").c_str(), "w");
		if (!plan)
			throw std::runtime_error("cannot write survey plan");
		fprintf(plan, "trial,hand,pose,x_right_m,y_up_m,z_forward_m,instruction,move_s,hold_s,return_s\n");
		for (size_t trial = 0; trial < counts.size(); ++trial) {
			const auto &pose = controller_survey_plan[trial % controller_survey_plan.size()];
			const size_t hand = trial / controller_survey_plan.size();
			fprintf(plan, "%zu,%zu,%s,%.3f,%.3f,%.3f,%s,3,5,5\n", trial, hand, pose.name,
			        (hand == 0 ? -1.f : 1.f) * pose.outward, pose.up, pose.forward, pose.instruction);
		}
		const bool plan_error = ferror(plan) != 0;
		if (fclose(plan) != 0 || plan_error)
			throw std::runtime_error("survey plan write failed");
		FILE *readme = fopen((directory + "/README.txt").c_str(), "w");
		if (!readme)
			throw std::runtime_error("cannot write survey metadata");
		fprintf(
		    readme,
		    "PSVR2 controller survey v3\n14 placements per selected hand; left then right for both.\n"
		    "3 s move, 5 s hold, 5 s return to front. Trigger starts each trial.\n"
		    "Primary face button pauses, secondary skips; either hand works.\n"
		    "Hold right Options for 1 s to recenter the service and reset the survey reference; current trial "
		    "returns to readiness.\n"
		    "The initial survey frame stays fixed: glance at the target during readiness, then look at the "
		    "blue cross.\n"
		    "Starting requires facing the cross within 15 degrees; hold phases still record head deviations.\n"
		    "Targets are requested placements, NOT calibration ground truth.\n"
		    "Poses/velocities use OpenXR LOCAL app space and predicted display time.\n"
		    "hand: 0=left, 1=right; kind: 0=grip, 1=aim. Complete events use hand=-1.\n"
		    "Invalid pose/velocity components are NaN; inspect their individual validity flags.\n"
		    "host_uptime_ns uses CLOCK_UPTIME_RAW; join service/CTD timestamps on that clock.\n"
		    "summary.csv counts selected-hand hold frames, not time-weighted coverage.\n"
		    "Samples are rendered frames; sensor evidence during focus/render gaps requires the service CTD.\n"
		    "Enable CONSTELLATION_TRACKER_DATA_RECORDER_OUTPUT in the SERVICE before starting it\n"
		    "to collect camera/blob/IMU data. This client cannot enable the service recorder.\n");
		fprintf(readme, "Selected hands: %s\nPassthrough requested: %d\n", hands, passthrough);
		const bool readme_error = ferror(readme) != 0;
		if (fclose(readme) != 0 || readme_error)
			throw std::runtime_error("survey metadata write failed");
	}

	void
	event(int64_t now, int64_t xr_time, const char *event_name, const controller_survey_state *observed = nullptr)
	{
		if (!events)
			return;
		const auto &s = observed ? *observed : state;
		const bool complete = s.phase == controller_survey_phase::complete;
		fprintf(events, "%lld,%lld,%zu,%d,%s,%s,%s\n", (long long)now, (long long)xr_time, s.trial,
		        complete ? -1 : (int)s.hand(),
		        complete ? "complete" : controller_survey_plan[s.pose_index()].name,
		        controller_survey_phase_name(s.phase), event_name);
		if (fflush(events) != 0)
			throw std::runtime_error("survey event write failed");
	}

	bool
	close()
	{
		if (!samples && !events)
			return !io_failed;
		if (samples) {
			io_failed |= ferror(samples) != 0;
			io_failed |= fclose(samples) != 0;
		}
		if (events) {
			io_failed |= ferror(events) != 0;
			io_failed |= fclose(events) != 0;
		}
		samples = events = nullptr;
		FILE *summary = fopen((directory + "/summary.csv").c_str(), "w");
		if (!summary) {
			io_failed = true;
			return false;
		}
		fprintf(summary,
		        "trial,hand,pose,hold_frames,active_frames,position_valid_frames,position_tracked_frames\n");
		for (size_t trial = 0; trial < counts.size(); ++trial) {
			const auto &c = counts[trial];
			fprintf(summary, "%zu,%zu,%s,%llu,%llu,%llu,%llu\n", trial,
			        trial / controller_survey_plan.size(),
			        controller_survey_plan[trial % controller_survey_plan.size()].name,
			        (unsigned long long)c[0], (unsigned long long)c[1], (unsigned long long)c[2],
			        (unsigned long long)c[3]);
		}
		io_failed |= ferror(summary) != 0;
		io_failed |= fclose(summary) != 0;
		return !io_failed;
	}
};
