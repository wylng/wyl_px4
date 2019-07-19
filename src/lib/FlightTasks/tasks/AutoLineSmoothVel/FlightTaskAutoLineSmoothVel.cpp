/****************************************************************************
 *
 *   Copyright (c) 2018 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file FlightAutoLine.cpp
 */

#include "FlightTaskAutoLineSmoothVel.hpp"
#include <mathlib/mathlib.h>
#include <float.h>

using namespace matrix;

bool FlightTaskAutoLineSmoothVel::activate(vehicle_local_position_setpoint_s last_setpoint)
{
	bool ret = FlightTaskAutoMapper2::activate(last_setpoint);

	checkSetpoints(last_setpoint);
	const Vector3f accel_prev(last_setpoint.acc_x, last_setpoint.acc_y, last_setpoint.acc_z);
	const Vector3f vel_prev(last_setpoint.vx, last_setpoint.vy, last_setpoint.vz);
	const Vector3f pos_prev(last_setpoint.x, last_setpoint.y, last_setpoint.z);

	for (int i = 0; i < 3; ++i) {
		_trajectory[i].reset(accel_prev(i), vel_prev(i), pos_prev(i));
	}

	_yaw_sp_prev = last_setpoint.yaw;
	_updateTrajConstraints();
	_initEkfResetCounters();

	return ret;
}

void FlightTaskAutoLineSmoothVel::reActivate()
{
	// On ground, reset acceleration and velocity to zero
	for (int i = 0; i < 2; ++i) {
		_trajectory[i].reset(0.f, 0.f, _position(i));
	}

	_trajectory[2].reset(0.f, 0.7f, _position(2));
	_initEkfResetCounters();
}

void FlightTaskAutoLineSmoothVel::checkSetpoints(vehicle_local_position_setpoint_s &setpoints)
{
	// If the position setpoint is unknown, set to the current postion
	if (!PX4_ISFINITE(setpoints.x)) { setpoints.x = _position(0); }

	if (!PX4_ISFINITE(setpoints.y)) { setpoints.y = _position(1); }

	if (!PX4_ISFINITE(setpoints.z)) { setpoints.z = _position(2); }

	// If the velocity setpoint is unknown, set to the current velocity
	if (!PX4_ISFINITE(setpoints.vx)) { setpoints.vx = _velocity(0); }

	if (!PX4_ISFINITE(setpoints.vy)) { setpoints.vy = _velocity(1); }

	if (!PX4_ISFINITE(setpoints.vz)) { setpoints.vz = _velocity(2); }

	// No acceleration estimate available, set to zero if the setpoint is NAN
	if (!PX4_ISFINITE(setpoints.acc_x)) { setpoints.acc_x = 0.f; }

	if (!PX4_ISFINITE(setpoints.acc_y)) { setpoints.acc_y = 0.f; }

	if (!PX4_ISFINITE(setpoints.acc_z)) { setpoints.acc_z = 0.f; }

	if (!PX4_ISFINITE(setpoints.yaw)) { setpoints.yaw = _yaw; }
}

void FlightTaskAutoLineSmoothVel::_generateSetpoints()
{
	_prepareSetpoints();
	_generateTrajectory();

	if (!PX4_ISFINITE(_yaw_setpoint) && !PX4_ISFINITE(_yawspeed_setpoint)) {
		// no valid heading -> generate heading in this flight task
		_generateHeading();
	}
}

void FlightTaskAutoLineSmoothVel::_generateHeading()
{
	// Generate heading along trajectory if possible, otherwise hold the previous yaw setpoint
	if (!_generateHeadingAlongTraj()) {
		_yaw_setpoint = _yaw_sp_prev;
	}
}

bool FlightTaskAutoLineSmoothVel::_generateHeadingAlongTraj()
{
	bool res = false;
	Vector2f vel_sp_xy(_velocity_setpoint);
	Vector2f traj_to_target = Vector2f(_target) - Vector2f(_position);

	if ((vel_sp_xy.length() > .1f) &&
	    (traj_to_target.length() > _target_acceptance_radius)) {
		// Generate heading from velocity vector, only if it is long enough
		// and if the drone is far enough from the target
		_compute_heading_from_2D_vector(_yaw_setpoint, vel_sp_xy);
		res = true;
	}

	return res;
}

/* Constrain some value vith a constrain depending on the sign of the constrain
 * Example: 	- if the constrain is -5, the value will be constrained between -5 and 0
 * 		- if the constrain is 5, the value will be constrained between 0 and 5
 */
inline float FlightTaskAutoLineSmoothVel::_constrainOneSide(float val, float constrain)
{
	const float min = (constrain < FLT_EPSILON) ? constrain : 0.f;
	const float max = (constrain > FLT_EPSILON) ? constrain : 0.f;

	return math::constrain(val, min, max);
}

void FlightTaskAutoLineSmoothVel::_initEkfResetCounters()
{
	_reset_counters.xy = _sub_vehicle_local_position->get().xy_reset_counter;
	_reset_counters.vxy = _sub_vehicle_local_position->get().vxy_reset_counter;
	_reset_counters.z = _sub_vehicle_local_position->get().z_reset_counter;
	_reset_counters.vz = _sub_vehicle_local_position->get().vz_reset_counter;
}

void FlightTaskAutoLineSmoothVel::_checkEkfResetCounters()
{
	// Check if a reset event has happened.
	if (_sub_vehicle_local_position->get().xy_reset_counter != _reset_counters.xy) {
		_trajectory[0].setCurrentPosition(_position(0));
		_trajectory[1].setCurrentPosition(_position(1));
		_reset_counters.xy = _sub_vehicle_local_position->get().xy_reset_counter;
	}

	if (_sub_vehicle_local_position->get().vxy_reset_counter != _reset_counters.vxy) {
		_trajectory[0].setCurrentVelocity(_velocity(0));
		_trajectory[1].setCurrentVelocity(_velocity(1));
		_reset_counters.vxy = _sub_vehicle_local_position->get().vxy_reset_counter;
	}

	if (_sub_vehicle_local_position->get().z_reset_counter != _reset_counters.z) {
		_trajectory[2].setCurrentPosition(_position(2));
		_reset_counters.z = _sub_vehicle_local_position->get().z_reset_counter;
	}

	if (_sub_vehicle_local_position->get().vz_reset_counter != _reset_counters.vz) {
		_trajectory[2].setCurrentVelocity(_velocity(2));
		_reset_counters.vz = _sub_vehicle_local_position->get().vz_reset_counter;
	}
}

float FlightTaskAutoLineSmoothVel::_getSpeedAtTarget()
{
	// Compute the maximum allowed speed at the waypoint assuming that we want to connect the two lines (prev-current and current-next)
	// with a tangent circle with constant speed and desired centripetal acceleration: a_centripetal = speed^2 / radius
	// The circle should in theory start and end at the intersection of the lines and the waypoint's acceptance radius. This is not
	// exactly true in reality since Navigator switches the waypoint so we have to take in account that the real acceptance radius is smaller.
	// It can be that the next waypoint is the last one or that the drone will have to stop for some other reason so we have to make sure
	// that the speed at the current waypoint allows to stop at the next waypoint.
	float speed_at_target = 0.0f;

	const float distance_current_next = Vector2f(&(_target - _next_wp)(0)).length();
	const bool waypoint_overlap = Vector2f(&(_target - _prev_wp)(0)).length() < _target_acceptance_radius;
	const bool yaw_align_check_pass = (_param_mpc_yaw_mode.get() != 4) || _yaw_sp_aligned;

	if (distance_current_next > 0.001f &&
	    !waypoint_overlap &&
	    yaw_align_check_pass) {
		// Max speed between current and next
		const float max_speed_current_next = _getMaxSpeedFromDistance(distance_current_next);
		const float alpha = acos(Vector2f(&(_target - _prev_wp)(0)).unit_or_zero() * Vector2f(&(_target - _next_wp)(
						 0)).unit_or_zero()) / 2.f;
		const float tan_alpha = tan(alpha);
		// We choose a maximum centripetal acceleration of MPC_ACC_HOR/2 to take in account that there is a jerk limit (a direct transition from line to circle is not possible)
		// We assume that the real radius of the acceptance radius is half of the _target_acceptance_radius since Navigator switches for us depending on the current position of
		// the drone. This allows for some tolerance on tracking error.
		speed_at_target = math::min(math::min(sqrtf(_param_mpc_acc_hor.get() / 2.f * _target_acceptance_radius / 2.f *
						      tan_alpha), max_speed_current_next), _mc_cruise_speed);
	}

	return speed_at_target;
}

float FlightTaskAutoLineSmoothVel::_getMaxSpeedFromDistance(float braking_distance)
{
	// Compute the maximum possible velocity on the track given the remaining distance, the maximum acceleration and the maximum jerk.
	// We assume a constant acceleration profile with a delay of 2*accel/jerk (time to reach the desired acceleration from opposite max acceleration)
	// Equation to solve: 0 = vel^2 - 2*acc*(x - vel*2*acc/jerk)
	// To avoid high gain at low distance due to the sqrt, we take the minimum of this velocity and a slope of "traj_p" m/s per meter
	float b = 4.f * _param_mpc_acc_hor.get() * _param_mpc_acc_hor.get() / _param_mpc_jerk_auto.get();
	float c = - 2.f * _param_mpc_acc_hor.get() * braking_distance;
	float max_speed = 0.5f * (-b + sqrtf(b * b - 4.f * c));
	max_speed = math::min(max_speed, braking_distance * _param_mpc_xy_traj_p.get());

	return max_speed;
}

void FlightTaskAutoLineSmoothVel::_prepareSetpoints()
{
	// Interface: A valid position setpoint generates a velocity target using a P controller. If a velocity is specified
	// that one is used as a velocity limit.
	// If the position setpoints are set to NAN, the values in the velocity setpoints are used as velocity targets: nothing to do here.

	_checkEkfResetCounters();
	_want_takeoff = false;

	if (_param_mpc_yaw_mode.get() == 4 && !_yaw_sp_aligned) {
		// Wait for the yaw setpoint to be aligned
		_velocity_setpoint.setAll(0.f);

	} else {
		if (PX4_ISFINITE(_position_setpoint(0)) &&
		    PX4_ISFINITE(_position_setpoint(1))) {
			// Use position setpoints to generate velocity setpoints

			// Get various path specific vectors. */
			Vector3f pos_traj;
			pos_traj(0) = _trajectory[0].getCurrentPosition();
			pos_traj(1) = _trajectory[1].getCurrentPosition();
			pos_traj(2) = _trajectory[2].getCurrentPosition();
			Vector2f pos_traj_to_dest_xy(_position_setpoint - pos_traj);
			Vector2f u_pos_traj_to_dest_xy(pos_traj_to_dest_xy.unit_or_zero());
			const bool has_reached_altitude = fabsf(_position_setpoint(2) - pos_traj(2)) < _param_nav_mc_alt_rad.get();

			float speed_sp_track = _getMaxSpeedFromDistance(pos_traj_to_dest_xy.length());

			if (has_reached_altitude) {
				speed_sp_track = math::constrain(speed_sp_track, _getSpeedAtTarget(), _mc_cruise_speed);

			} else {
				speed_sp_track = math::constrain(speed_sp_track, 0.0f, _mc_cruise_speed);

			}

			Vector2f vel_sp_xy = u_pos_traj_to_dest_xy * speed_sp_track;

			for (int i = 0; i < 2; i++) {
				// If available, constrain the velocity using _velocity_setpoint(.)
				if (PX4_ISFINITE(_velocity_setpoint(i))) {
					_velocity_setpoint(i) = _constrainOneSide(vel_sp_xy(i), _velocity_setpoint(i));

				} else {
					_velocity_setpoint(i) = vel_sp_xy(i);
				}
			}

		}

		if (PX4_ISFINITE(_position_setpoint(2))) {
			const float vel_sp_z = (_position_setpoint(2) - _trajectory[2].getCurrentPosition()) *
					       _param_mpc_z_traj_p.get(); // Generate a velocity target for the trajectory using a simple P loop

			// If available, constrain the velocity using _velocity_setpoint(.)
			if (PX4_ISFINITE(_velocity_setpoint(2))) {
				_velocity_setpoint(2) = _constrainOneSide(vel_sp_z, _velocity_setpoint(2));

			} else {
				_velocity_setpoint(2) = vel_sp_z;
			}

			_want_takeoff = _velocity_setpoint(2) < -0.3f;
		}
	}
}

void FlightTaskAutoLineSmoothVel::_updateTrajConstraints()
{
	// Update the constraints of the trajectories
	_trajectory[0].setMaxAccel(_param_mpc_acc_hor.get()); // TODO : Should be computed using heading
	_trajectory[1].setMaxAccel(_param_mpc_acc_hor.get());
	_trajectory[0].setMaxVel(_param_mpc_xy_vel_max.get());
	_trajectory[1].setMaxVel(_param_mpc_xy_vel_max.get());
	_trajectory[0].setMaxJerk(_param_mpc_jerk_auto.get()); // TODO : Should be computed using heading
	_trajectory[1].setMaxJerk(_param_mpc_jerk_auto.get());
	_trajectory[2].setMaxJerk(_param_mpc_jerk_auto.get());

	if (_velocity_setpoint(2) < 0.f) { // up
		_trajectory[2].setMaxAccel(_param_mpc_acc_up_max.get());
		_trajectory[2].setMaxVel(_param_mpc_z_vel_max_up.get());

	} else { // down
		_trajectory[2].setMaxAccel(_param_mpc_acc_down_max.get());
		_trajectory[2].setMaxVel(_param_mpc_z_vel_max_dn.get());
	}
}

void FlightTaskAutoLineSmoothVel::_generateTrajectory()
{
	if (!PX4_ISFINITE(_velocity_setpoint(0)) || !PX4_ISFINITE(_velocity_setpoint(1))
	    || !PX4_ISFINITE(_velocity_setpoint(2))) {
		return;
	}

	/* Slow down the trajectory by decreasing the integration time based on the position error.
	 * This is only performed when the drone is behind the trajectory
	 */
	Vector2f position_trajectory_xy(_trajectory[0].getCurrentPosition(), _trajectory[1].getCurrentPosition());
	Vector2f position_xy(_position);
	Vector2f vel_traj_xy(_trajectory[0].getCurrentVelocity(), _trajectory[1].getCurrentVelocity());
	Vector2f drone_to_trajectory_xy(position_trajectory_xy - position_xy);
	float position_error = drone_to_trajectory_xy.length();

	float time_stretch = 1.f - math::constrain(position_error * 0.5f, 0.f, 1.f);

	// Don't stretch time if the drone is ahead of the position setpoint
	if (drone_to_trajectory_xy.dot(vel_traj_xy) < 0.f) {
		time_stretch = 1.f;
	}

	Vector3f jerk_sp_smooth;
	Vector3f accel_sp_smooth;
	Vector3f vel_sp_smooth;
	Vector3f pos_sp_smooth;

	for (int i = 0; i < 3; ++i) {
		_trajectory[i].integrate(_deltatime, time_stretch, accel_sp_smooth(i), vel_sp_smooth(i), pos_sp_smooth(i));
		jerk_sp_smooth(i) = _trajectory[i].getCurrentJerk();
	}

	_updateTrajConstraints();

	// If the acceleration and velocities are small and that we want to stop, reduce the amplitude of the jerk signal
	// to help the optimizer to converge towards zero
	if (Vector2f(_velocity_setpoint).length() < (0.01f * _param_mpc_xy_traj_p.get())
	    && Vector2f(accel_sp_smooth).length() < 0.2f
	    && Vector2f(vel_sp_smooth).length() < 0.1f) {
		_trajectory[0].setMaxJerk(1.f);
		_trajectory[1].setMaxJerk(1.f);
	}

	for (int i = 0; i < 3; ++i) {
		_trajectory[i].updateDurations(_deltatime, _velocity_setpoint(i));
	}

	VelocitySmoothing::timeSynchronization(_trajectory, 2); // Synchronize x and y only

	_jerk_setpoint = jerk_sp_smooth;
	_acceleration_setpoint = accel_sp_smooth;
	_velocity_setpoint = vel_sp_smooth;
	_position_setpoint = pos_sp_smooth;
}
