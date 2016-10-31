#include "quaternion_pd_controller.h"

QuaternionPdController::QuaternionPdController()
{
  stateSub    = nh.subscribe("state_estimate", 10, &QuaternionPdController::stateCallback, this);
  setpointSub = nh.subscribe("pose_setpoints", 10, &QuaternionPdController::setpointCallback, this);
  controlPub  = nh.advertise<geometry_msgs::Wrench>("rov_forces", 10);
  enabled = false;

  p.setZero();
  q.setIdentity();
  nu.setZero();
  p_d.setZero();
  q_d.setIdentity();
  z.setZero();
  tau.setZero();
  g.setZero();
  R = q.toRotationMatrix();

  getParams();
}

void QuaternionPdController::stateCallback(const nav_msgs::Odometry &msg)
{
  tf::pointMsgToEigen(msg.pose.pose.position, p);
  tf::quaternionMsgToEigen(msg.pose.pose.orientation, q);
  tf::twistMsgToEigen(msg.twist.twist, nu);
  q.normalize();
  R = q.toRotationMatrix();

  if (isFucked(p) || isFucked(nu) || isFucked(R))
    ROS_WARN("p, nu, or R is fucked.");
}

void QuaternionPdController::setpointCallback(const geometry_msgs::Pose &msg)
{
  tf::pointMsgToEigen(msg.position, p_d);
  tf::quaternionMsgToEigen(msg.orientation, q_d);
  q_d.normalize();

  if (isFucked(p_d))
    ROS_WARN("p_d is fucked.");
}

void QuaternionPdController::setGains(double a_new, double b_new, double c_new)
{
  a = a_new;
  b = b_new;
  c = c_new;
  K_D = a * Eigen::MatrixXd::Identity(6,6);
  K_p = b * Eigen::MatrixXd::Identity(3,3);
}

void QuaternionPdController::compute()
{
  if (enabled)
  {
    updateProportionalGainMatrix();
    updateErrorVector();
    updateRestoringForceVector();

    tau = - K_D*nu - K_P*z + g;

    if (isFucked(tau))
      ROS_WARN("tau is fucked.");

    geometry_msgs::Wrench msg;
    tf::wrenchEigenToMsg(tau, msg);
    controlPub.publish(msg);
  }
}

void QuaternionPdController::enable()
{
  enabled = true;
}

void QuaternionPdController::disable()
{
  enabled = false;
}

void QuaternionPdController::updateProportionalGainMatrix()
{
  K_P << R.transpose() * K_p,        Eigen::MatrixXd::Zero(3,3),
         Eigen::MatrixXd::Zero(3,3), c*Eigen::MatrixXd::Identity(3,3);

  if (isFucked(K_P))
    ROS_WARN("K_P is fucked.");
}

void QuaternionPdController::updateErrorVector()
{
  Eigen::Vector3d p_tilde = p - p_d;

  if (isFucked(p_tilde))
    ROS_WARN("p_tilde is fucked.");

  Eigen::Quaterniond q_tilde = q_d.conjugate()*q;
  q_tilde.normalize();

  z << p_tilde,
     sgn(q_tilde.w())*q_tilde.vec();

  if (isFucked(z))
    ROS_WARN("z is fucked.");
}

void QuaternionPdController::updateRestoringForceVector()
{
  Eigen::Vector3d f_g = R.transpose() * Eigen::Vector3d(0, 0, W);
  Eigen::Vector3d f_b = R.transpose() * Eigen::Vector3d(0, 0, -B);

  g << f_g + f_b,
       r_g.cross(f_g) + r_b.cross(f_b);

  if (isFucked(g))
    ROS_WARN("g is fucked.");
}

int QuaternionPdController::sgn(double x)
{
  if (x < 0)
    return -1;
  return 1;
}

void QuaternionPdController::getParams()
{
  // Read controller gains from parameter server
  double a_init, b_init, c_init;
  if (!nh.getParam("/controller/gains/a", a_init))
    ROS_ERROR("Failed to read derivative controller gain (a).");
  if (!nh.getParam("/controller/gains/b", b_init))
    ROS_ERROR("Failed to read position controller gain (b).");
  if (!nh.getParam("/controller/gains/c", c_init))
    ROS_ERROR("Failed to read orientation controller gain (c).");
  setGains(a_init, b_init, c_init);

  // Read and calculate ROV weight and buoyancy
  double mass, displacement, acceleration_of_gravity, density_of_water;
  if (!nh.getParam("/physical/mass_kg", mass))
    ROS_ERROR("Failed to read parameter mass.");
  if (!nh.getParam("/physical/displacement_m3", displacement))
    ROS_ERROR("Failed to read parameter displacement.");
  if (!nh.getParam("/gravity/acceleration", acceleration_of_gravity))
    ROS_ERROR("Failed to read parameter acceleration of gravity");
  if (!nh.getParam("/water/density", density_of_water))
    ROS_ERROR("Failed to read parameter density of water");
  W = mass * acceleration_of_gravity;
  B = density_of_water * displacement * acceleration_of_gravity;

  // Read center of gravity and buoyancy vectors
  std::vector<double> r_g_vec, r_b_vec;
  if (!nh.getParam("/physical/center_of_mass", r_g_vec))
    ROS_ERROR("Failed to read robot center of mass parameter.");
  if (!nh.getParam("/physical/center_of_buoyancy", r_b_vec))
    ROS_ERROR("Failed to read robot center of buoyancy parameter.");
  r_g << r_g_vec[0], r_g_vec[1], r_g_vec[2];
  r_b << r_b_vec[0], r_b_vec[1], r_b_vec[2];
}
