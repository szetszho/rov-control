#include "ros/ros.h"
#include "quaternion_pd_controller.h"
#include "open_loop_controller.h"
#include "uranus_dp/SetControlMode.h"
#include "uranus_dp/SetControllerGains.h"
#include "../control_mode_enum.h"

class Controller
{
public:
    Controller(unsigned int f)
    {
        frequency = f;
        control_mode = ControlModes::OPEN_LOOP;
        position_hold_controller.disable();
        open_loop_controller.enable();
    }

    bool setControlMode(uranus_dp::SetControlMode::Request &req, uranus_dp::SetControlMode::Response &resp)
    {
        ControlMode new_control_mode = static_cast<ControlMode>(req.mode);
        if (new_control_mode != control_mode)
        {
            control_mode = new_control_mode;
            switch (control_mode)
            {
            case ControlModes::OPEN_LOOP:
                ROS_INFO("Changing control mode to open loop.");
                position_hold_controller.disable();
                open_loop_controller.enable();
                break;
            case ControlModes::POSITION_HOLD:
                ROS_INFO("Changing control mode to hold position.");
                open_loop_controller.disable();
                position_hold_controller.enable();
                break;
            default:
                ROS_WARN("Invalid control mode set.");
                break;
            }
        }
        else
        {
            ROS_INFO("Attempt to set already active control mode, ignoring.");
        }
        return true;
    }

    bool setControllerGains(uranus_dp::SetControllerGains::Request &req, uranus_dp::SetControllerGains::Response &resp)
    {
        ROS_INFO_STREAM("Setting new gains: a = " << req.a << ", b = " << req.b << ", c = " << req.c << ".");
        position_hold_controller.setGains(req.a, req.b, req.c);
    }

    void run()
    {
        ros::Rate rate(frequency);
        while (ros::ok())
        {
            ros::spinOnce();
            position_hold_controller.compute();
            rate.sleep();
        }
    }
private:
    ros::NodeHandle nh;
    unsigned int frequency;

    ControlMode control_mode;
    QuaternionPdController position_hold_controller; // I hardly know her.
    OpenLoopController     open_loop_controller;
};



int main(int argc, char **argv)
{
    ros::init(argc, argv, "controller");
    ROS_INFO("Launching node controller.");
    ros::NodeHandle nh;

    unsigned int frequency = 10;
    Controller controller(frequency);
    ros::ServiceServer ss1 = nh.advertiseService("set_control_mode", &Controller::setControlMode, &controller);
    ros::ServiceServer ss2 = nh.advertiseService("set_controller_gains", &Controller::setControllerGains, &controller);

    controller.run();
    ROS_ERROR("controller.run() has returned. That's not meant to happen.");
    return 0;
}
