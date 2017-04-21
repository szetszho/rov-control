#!/usr/bin/env python

import rospy
import ms5837

from sensor_msgs.msg import FluidPressure, Temperature

class Ms5837InterfaceNode(object):
    def __init__(self):
        rospy.init_node('pressure_node')
        self.ms5837 = ms5837.MS5837(model=ms5837.MODEL_30BA, bus=1)
        if not self.ms5837.init():
            rospy.logfatal('Failed to initialise MS5837! Is the sensor connected?')
        else:
            if not self.ms5837.read():
                rospy.logfatal('Failed to read MS5837!')
            else:
                rospy.loginfo('Successfully initialised MS5837')

        self.pub_pressure = rospy.Publisher(
            'sensors/pressure',
            FluidPressure,
            queue_size=10)

        self.pub_temp = rospy.Publisher(
            'sensors/temperature',
            Temperature,
            queue_size=10)


if __name__ == '__main__':
    try:
        pressure_node = Ms5837InterfaceNode()
        rospy.spin()
    except rospy.ROSInterruptException:
        pass
