#include "ros/ros.h"
#include "geometry_msgs/Pose.h"
#include "geometry_msgs/Twist.h"
#include "geometry_msgs/Wrench.h"
#include "uranus_dp/State.h"
#include <Eigen/Dense>
#include <cmath>

class Controller
{
public:
    Controller()
    {
        outputPub = n.advertise<geometry_msgs::Wrench>("outputTopic", 1);

        // I don't know why, but 'this' is necessary here
        stateSub = n.subscribe("stateTopic", 1, &Controller::stateCallback, this);
        setpointSub = n.subscribe("setpointTopic", 1, &Controller::setpointCallback, this);

        // Allocate dynamic member variables
        T = Eigen::MatrixXd(4,3);
        tau = Eigen::VectorXd(3);

        // Initialize controller gains
        K_p = Eigen::Matrix3d::Identity();
        K_d = Eigen::Matrix3d::Identity();
    }

    // stateCallback updates the private state variables when a new state message arrives.
    void stateCallback(const uranus_dp::State &state_msg)
    {
        // Copy position values
        p(0) = state_msg.pose.position.x;
        p(1) = state_msg.pose.position.y;
        p(2) = state_msg.pose.position.z;

        // Copy orientation values (quaternion)
        q(0) = state_msg.pose.orientation.x;
        q(1) = state_msg.pose.orientation.y;
        q(2) = state_msg.pose.orientation.z;
        q(3) = state_msg.pose.orientation.w;

        // Copy linear velocity values
        v(0) = state_msg.twist.linear.x;
        v(1) = state_msg.twist.linear.y;
        v(2) = state_msg.twist.linear.z;

        // Copy angular velocity values
        omega(0) = state_msg.twist.angular.x;
        omega(1) = state_msg.twist.angular.y;
        omega(2) = state_msg.twist.angular.z;
    }

    // setpointCallback updates the private setpoint variable when a new setpoint message arrives.
    void setpointCallback(const geometry_msgs::Twist &setpoint_msg)
    {
        // Copy linear velocity setpoint values
        v_sp(0) = setpoint_msg.linear.x;
        v_sp(1) = setpoint_msg.linear.y;
        v_sp(2) = setpoint_msg.linear.z;

        // Copy angular velocity setpoint values
        omega_sp(0) = setpoint_msg.angular.x;
        omega_sp(1) = setpoint_msg.angular.y;
        omega_sp(2) = setpoint_msg.angular.z;
    }

    // calculate contains the control algorithm, and calculates the control output based on the
    // current state and setpoint.
    void calculate(void)
    {
        // Only orientation control for now.

        // Orientation error
        q_err = q_sp - q;

        // Control output
        updateTransformationMatrices();
        tau = - K_d * omega - K_p * T.transpose() * q_err;

        // Build output message
        geometry_msgs::Wrench output;
        output.force.x = 0;
        output.force.y = 0;
        output.force.z = 0;
        output.torque.x = tau(0);
        output.torque.y = tau(1);
        output.torque.z = tau(2);
        outputPub.publish(output);
    }

    void updateTransformationMatrices(void)
    {
        // Linear velocity transformation matrix
        R(0,0) = 1 - 2*(pow(q(2),2) + pow(q(3),2));
        R(0,1) = 2*(q(1)*q(2) - q(3)*q(0));
        R(0,2) = 2*(q(1)*q(3) - q(2)*q(0));
        
        R(1,0) = 2*(q(1)*q(2) + q(3)*q(0));
        R(1,1) = 1 - 2*(pow(q(1),2) + pow(q(0),2));
        R(1,2) = 2*(q(2)*q(3) + q(1)*q(0));
        
        R(2,0) = 2*(q(1)*q(3) + q(2)*q(0));
        R(2,1) = 2*(q(2)*q(3) + q(1)*q(0));
        R(2,2) = 1 - 2*(pow(q(1),2) + pow(q(2),2));

        // Angular velocity transformation matrix
        T(0,0) = -q(1);
        T(0,1) = -q(2);
        T(0,2) = -q(3);

        T(1,0) =  q(0);
        T(1,1) = -q(3);
        T(1,2) =  q(2);

        T(2,0) =  q(3);
        T(2,1) =  q(3);
        T(2,2) = -q(1);

        T(3,0) = -q(2);
        T(3,1) =  q(1);
        T(3,2) =  q(0);

        T *= 0.5;
    } 
private:
    ros::NodeHandle n;

    ros::Publisher  outputPub;
    ros::Subscriber stateSub;
    ros::Subscriber setpointSub;

    // State
    Eigen::Vector3d p;     // Position
    Eigen::Vector4d q;     // Orientation
    Eigen::Vector3d v;     // Linear velocity
    Eigen::Vector3d omega; // Angular velocity

    // Setpoints
    Eigen::Vector3d p_sp;     // Position
    Eigen::Vector4d q_sp;     // Orientation
    Eigen::Vector3d v_sp;     // Linear velocity
    Eigen::Vector3d omega_sp; // Angular velocity

    // Errors
    Eigen::Vector3d p_err;     // Position
    Eigen::Vector4d q_err;     // Orientation
    Eigen::Vector3d v_err;     // Linear velocity
    Eigen::Vector3d omega_err; // Angular velocity

    // Transformation matrices
    Eigen::Matrix3d R;
    Eigen::MatrixXd T;

    // Control output
    Eigen::VectorXd tau;

    // Controller gains
    Eigen::Matrix3d K_d;
    Eigen::Matrix3d K_p;
}; // End of class Controller

int main(int argc, char **argv)
{
    ros::init(argc, argv, "controller");

    Controller controllerObject;

    // Run the controller at 10 Hz
    ros::Rate loop_rate(10);
    while (ros::ok())
    {
        ros::spinOnce();

        controllerObject.calculate();

        loop_rate.sleep();
    }

    return 0;
}
