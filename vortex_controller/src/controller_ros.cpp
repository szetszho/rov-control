#include "vortex_controller/controller_ros.h"

#include "vortex/eigen_helper.h"
#include <tf/transform_datatypes.h>
#include <eigen_conversions/eigen_msg.h>

#include "std_msgs/String.h"

#include <math.h>
#include <map>
#include <string>
#include <vector>

Controller::Controller(ros::NodeHandle nh) : nh(nh)
{
  command_sub = nh.subscribe("propulsion_command", 1, &Controller::commandCallback, this);
  state_sub   = nh.subscribe("state_estimate", 1, &Controller::stateCallback, this);
  wrench_pub  = nh.advertise<geometry_msgs::Wrench>("rov_forces", 1);
  mode_pub    = nh.advertise<std_msgs::String>("controller/mode", 10);

  control_mode = ControlModes::OPEN_LOOP;

  if (!nh.getParam("/controller/frequency", frequency))
  {
    ROS_WARN("Failed to read parameter controller frequency, defaulting to 10 Hz.");
    frequency = 10;
  }

  state = new State();
  initSetpoints();
  initPositionHoldController();

  // Set up a dynamic reconfigure server
  dynamic_reconfigure::Server<vortex_controller::VortexControllerConfig>::CallbackType dr_cb;
  dr_cb = boost::bind(&Controller::configCallback, this, _1, _2);
  dr_srv.setCallback(dr_cb);

  ROS_INFO("Initialized at %i Hz.", frequency);
}

void Controller::commandCallback(const vortex_msgs::PropulsionCommand& msg)
{
  if (!healthyMessage(msg))
    return;

  ControlMode new_control_mode;
  new_control_mode = control_mode;
  for (int i = 0; i < msg.control_mode.size(); ++i)
  {
    if (msg.control_mode[i])
    {
      new_control_mode = static_cast<ControlMode>(i);
      break;
    }
  }

  if (new_control_mode != control_mode)
  {
    control_mode = new_control_mode;
    resetSetpoints();
    ROS_INFO_STREAM("Changing mode to " << controlModeString(control_mode) << ".");
  }
  publishControlMode();

  double time = msg.header.stamp.toSec();
  Eigen::Vector6d command;
  for (int i = 0; i < 6; ++i)
    command(i) = msg.motion[i];
  setpoints->update(time, command);
}

void Controller::stateCallback(const nav_msgs::Odometry &msg)
{
  Eigen::Vector3d    position;
  Eigen::Quaterniond orientation;
  Eigen::Vector6d    velocity;

  tf::pointMsgToEigen(msg.pose.pose.position, position);
  tf::quaternionMsgToEigen(msg.pose.pose.orientation, orientation);
  tf::twistMsgToEigen(msg.twist.twist, velocity);

  bool orientation_invalid = (abs(orientation.norm() - 1) > MAX_QUAT_NORM_DEVIATION);
  if (isFucked(position) || isFucked(velocity) || orientation_invalid)
  {
    ROS_WARN_THROTTLE(1, "Invalid state estimate received, ignoring...");
    return;
  }

  state->set(position, orientation, velocity);
}

void Controller::configCallback(const vortex_controller::VortexControllerConfig &config, uint32_t level)
{
  ROS_INFO_STREAM("Setting gains: [vel = " << config.velocity_gain << ", pos = " << config.position_gain
    << ", rot = " << config.attitude_gain << "]");
  controller->setGains(config.velocity_gain, config.position_gain, config.attitude_gain);
}

void Controller::spin()
{
  Eigen::Vector6d    tau_command          = Eigen::VectorXd::Zero(6);
  Eigen::Vector6d    tau_openloop         = Eigen::VectorXd::Zero(6);
  Eigen::Vector6d    tau_restoring        = Eigen::VectorXd::Zero(6);
  Eigen::Vector6d    tau_staylevel        = Eigen::VectorXd::Zero(6);
  Eigen::Vector6d    tau_depthhold        = Eigen::VectorXd::Zero(6);
  Eigen::Vector6d    tau_headinghold      = Eigen::VectorXd::Zero(6);

  Eigen::Vector3d    position_state       = Eigen::Vector3d::Zero();
  Eigen::Quaterniond orientation_state    = Eigen::Quaterniond::Identity();
  Eigen::Vector6d    velocity_state       = Eigen::VectorXd::Zero(6);

  Eigen::Vector3d    position_setpoint    = Eigen::Vector3d::Zero();
  Eigen::Quaterniond orientation_setpoint = Eigen::Quaterniond::Identity();

  geometry_msgs::Wrench msg;

  ros::Rate rate(frequency);
  while (ros::ok())
  {
    // TODO(mortenfyhn): check value of bool return from getters
    state->get(&position_state, &orientation_state, &velocity_state);
    setpoints->get(&position_setpoint, &orientation_setpoint);
    setpoints->get(&tau_openloop);

    tau_command.setZero();

    switch (control_mode)
    {
      case ControlModes::OPEN_LOOP:
      tau_command = tau_openloop;
      break;

      case ControlModes::OPEN_LOOP_RESTORING:
      tau_restoring = controller->getRestoring(orientation_state);
      tau_command = tau_openloop + tau_restoring;
      break;

      case ControlModes::STAY_LEVEL:
      tau_staylevel = stayLevel(orientation_state, velocity_state);
      tau_command = tau_openloop + tau_staylevel;
      break;

      case ControlModes::DEPTH_HOLD:
      tau_depthhold = depthHold(tau_openloop,
                                position_state,
                                orientation_state,
                                velocity_state,
                                position_setpoint);
      tau_command = tau_openloop + tau_depthhold;
      break;

      case ControlModes::HEADING_HOLD:
      tau_headinghold = headingHold(tau_openloop,
                                    position_state,
                                    orientation_state,
                                    velocity_state,
                                    orientation_setpoint);
      tau_command = tau_openloop + tau_headinghold;
      break;

      case ControlModes::DEPTH_HEADING_HOLD:
      tau_depthhold = depthHold(tau_openloop,
                                position_state,
                                orientation_state,
                                velocity_state,
                                position_setpoint);
      tau_headinghold = headingHold(tau_openloop,
                                    position_state,
                                    orientation_state,
                                    velocity_state,
                                    orientation_setpoint);
      tau_command = tau_openloop + tau_depthhold + tau_headinghold;
      break;

      default:
      ROS_ERROR("Default control mode reached.");
      break;
    }

    tf::wrenchEigenToMsg(tau_command, msg);
    wrench_pub.publish(msg);

    ros::spinOnce();
    rate.sleep();
  }
}

void Controller::initSetpoints()
{
  std::vector<double> v;

  if (!nh.getParam("/propulsion/command/wrench/max", v))
    ROS_FATAL("Failed to read parameter max wrench command.");
  Eigen::Vector6d wrench_command_max = Eigen::Vector6d::Map(v.data(), v.size());

  if (!nh.getParam("/propulsion/command/wrench/scaling", v))
    ROS_FATAL("Failed to read parameter scaling wrench command.");
  Eigen::Vector6d wrench_command_scaling = Eigen::Vector6d::Map(v.data(), v.size());

  if (!nh.getParam("/propulsion/command/pose/rate", v))
    ROS_FATAL("Failed to read parameter pose command rate.");
  Eigen::Vector6d pose_command_rate = Eigen::Vector6d::Map(v.data(), v.size());

  setpoints = new Setpoints(wrench_command_scaling,
                            wrench_command_max,
                            pose_command_rate);
}

void Controller::resetSetpoints()
{
  // Reset setpoints to be equal to state
  Eigen::Vector3d position;
  Eigen::Quaterniond orientation;
  state->get(&position, &orientation);
  setpoints->set(position, orientation);
}

void Controller::initPositionHoldController()
{
  // Read controller gains from parameter server
  double a, b, c;
  if (!nh.getParam("/controller/velocity_gain", a))
    ROS_ERROR("Failed to read parameter velocity_gain.");
  if (!nh.getParam("/controller/position_gain", b))
    ROS_ERROR("Failed to read parameter position_gain.");
  if (!nh.getParam("/controller/attitude_gain", c))
    ROS_ERROR("Failed to read parameter attitude_gain.");

  // Read center of gravity and buoyancy vectors
  std::vector<double> r_G_vec, r_B_vec;
  if (!nh.getParam("/physical/center_of_mass", r_G_vec))
    ROS_FATAL("Failed to read robot center of mass parameter.");
  if (!nh.getParam("/physical/center_of_buoyancy", r_B_vec))
    ROS_FATAL("Failed to read robot center of buoyancy parameter.");
  Eigen::Vector3d r_G(r_G_vec.data());
  Eigen::Vector3d r_B(r_B_vec.data());

  // Read and calculate ROV weight and buoyancy
  double mass, displacement, acceleration_of_gravity, density_of_water;
  if (!nh.getParam("/physical/mass_kg", mass))
    ROS_FATAL("Failed to read parameter mass.");
  if (!nh.getParam("/physical/displacement_m3", displacement))
    ROS_FATAL("Failed to read parameter displacement.");
  if (!nh.getParam("/gravity/acceleration", acceleration_of_gravity))
    ROS_FATAL("Failed to read parameter acceleration of gravity");
  if (!nh.getParam("/water/density", density_of_water))
    ROS_FATAL("Failed to read parameter density of water");
  double W = mass * acceleration_of_gravity;
  double B = density_of_water * displacement * acceleration_of_gravity;

  controller = new QuaternionPdController(a, b, c, W, B, r_G, r_B);
}

bool Controller::healthyMessage(const vortex_msgs::PropulsionCommand& msg)
{
  // Check that motion commands are in range
  for (int i = 0; i < msg.motion.size(); ++i)
  {
    if (msg.motion[i] > 1 || msg.motion[i] < -1)
    {
      ROS_WARN("Motion command out of range, ignoring message...");
      return false;
    }
  }

  // Check correct length of control mode vector
  if (msg.control_mode.size() != ControlModes::CONTROL_MODE_END)
  {
    ROS_WARN_STREAM_THROTTLE(1, "Control mode vector has " << msg.control_mode.size()
      << " element(s), should have " << ControlModes::CONTROL_MODE_END);
    return false;
  }

  // Check that exactly zero or one control mode is requested
  int num_requested_modes = 0;
  for (int i = 0; i < msg.control_mode.size(); ++i)
    if (msg.control_mode[i])
      num_requested_modes++;
  if (num_requested_modes > 1)
  {
    ROS_WARN_STREAM("Attempt to set " << num_requested_modes << " control modes at once, ignoring message...");
    return false;
  }

  return true;
}

void Controller::publishControlMode()
{
  std::string s = controlModeString(control_mode);
  std_msgs::String msg;
  msg.data = s;
  mode_pub.publish(msg);
}

Eigen::Vector6d Controller::stayLevel(const Eigen::Quaterniond &orientation_state,
                                      const Eigen::Vector6d &velocity_state)
{
  // Convert quaternion setpoint to euler angles (ZYX convention)
  Eigen::Vector3d euler;
  euler = orientation_state.toRotationMatrix().eulerAngles(2, 1, 0);

  // Set pitch and roll setpoints to zero
  euler(EULER_PITCH) = 0;
  euler(EULER_ROLL)  = 0;

  // Convert euler setpoint back to quaternions
  Eigen::Matrix3d R;
  R = Eigen::AngleAxisd(euler(EULER_YAW),   Eigen::Vector3d::UnitZ())
    * Eigen::AngleAxisd(euler(EULER_PITCH), Eigen::Vector3d::UnitY())
    * Eigen::AngleAxisd(euler(EULER_ROLL),  Eigen::Vector3d::UnitX());
  Eigen::Quaterniond orientation_staylevel(R);

  Eigen::Vector6d tau = controller->getFeedback(Eigen::Vector3d::Zero(), orientation_state, velocity_state,
                                                Eigen::Vector3d::Zero(), orientation_staylevel);

  tau(WRENCH_ROLL)  = 0;
  tau(WRENCH_PITCH) = 0;

  return tau;
}

Eigen::Vector6d Controller::depthHold(const Eigen::Vector6d &tau_openloop,
                                      const Eigen::Vector3d &position_state,
                                      const Eigen::Quaterniond &orientation_state,
                                      const Eigen::Vector6d &velocity_state,
                                      const Eigen::Vector3d &position_setpoint)
{
  Eigen::Vector6d tau;

  bool activate_depthhold = fabs(tau_openloop(WRENCH_HEAVE)) < NORMALIZED_FORCE_DEADZONE;
  if (activate_depthhold)
  {
    tau = controller->getFeedback(position_state, Eigen::Quaterniond::Identity(), velocity_state,
                                  position_setpoint, Eigen::Quaterniond::Identity());

    // Allow only heave feedback command
    tau(WRENCH_SURGE) = 0;
    tau(WRENCH_SWAY)  = 0;
    tau(WRENCH_ROLL)  = 0;
    tau(WRENCH_PITCH) = 0;
    tau(WRENCH_YAW)   = 0;
  }
  else
  {
    resetSetpoints();
    tau.setZero();
  }

  return tau;
}

Eigen::Vector6d Controller::headingHold(const Eigen::Vector6d &tau_openloop,
                                        const Eigen::Vector3d &position_state,
                                        const Eigen::Quaterniond &orientation_state,
                                        const Eigen::Vector6d &velocity_state,
                                        const Eigen::Quaterniond &orientation_setpoint)
{
  Eigen::Vector6d tau;

  bool activate_headinghold = fabs(tau_openloop(WRENCH_YAW)) < NORMALIZED_FORCE_DEADZONE;
  if (activate_headinghold)
  {
    tau = controller->getFeedback(Eigen::Vector3d::Zero(), orientation_state, velocity_state,
                                  Eigen::Vector3d::Zero(), orientation_setpoint);

    // Allow only yaw feedback command
    tau(WRENCH_SURGE) = 0;
    tau(WRENCH_SWAY)  = 0;
    tau(WRENCH_HEAVE) = 0;
    tau(WRENCH_ROLL)  = 0;
    tau(WRENCH_PITCH) = 0;
  }
  else
  {
    resetSetpoints();
    tau.setZero();
  }

  return tau;
}
