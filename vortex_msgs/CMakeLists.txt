cmake_minimum_required(VERSION 2.8.3)
project(vortex_msgs)

find_package(catkin REQUIRED COMPONENTS
  message_generation
  geometry_msgs
  std_msgs
)

add_message_files(
  DIRECTORY msg
  FILES
  Float64ArrayStamped.msg
  LightInput.msg
  PropulsionCommand.msg
  EulerOrientations.msg
  Debug.msg
  Pwm.msg
  ContainerID.msg
  Manipulator.msg
  CameraFeedSelection.msg
)

generate_messages(
  DEPENDENCIES
  geometry_msgs
  std_msgs
)

catkin_package(
  CATKIN_DEPENDS
  message_runtime
  geometry_msgs
  std_msgs
)
