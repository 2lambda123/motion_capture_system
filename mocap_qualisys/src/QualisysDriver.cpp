/*
 * Copyright [2015]
 * [Kartik Mohta <kartikmohta@gmail.com>]
 * [Ke Sun <sunke.polyu@gmail.com>]
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cmath>
#include <Eigen/Dense>
#include <Eigen/Geometry>
#include <tf_conversions/tf_eigen.h>
#include <mocap_qualisys/QualisysDriver.h>

using namespace std;
using namespace Eigen;

namespace mocap{

double QualisysDriver::deg2rad = M_PI / 180.0;

bool QualisysDriver::init() {
  // The base port (as entered in QTM, TCP/IP port number, in the RT output tab
  // of the workspace options
  nh.param("server_address", server_address, string("192.168.254.1"));
  nh.param("server_base_port", base_port, 22222);
  nh.param("model_list", model_list, vector<string>(0));
  nh.param("frame_rate", frame_rate, 100);
  nh.param("max_accel", max_accel, 10.0);
  nh.param("publish_tf", publish_tf, false);
  nh.param("fixed_frame_id", fixed_frame_id, string("mocap"));
  // nh.param("worker_threads",  worker_threads, 8);

  frame_interval = 1.0 / static_cast<double>(frame_rate);
  double& dt = frame_interval;
  process_noise.topLeftCorner<6, 6>() =
    0.5*Matrix<double, 6, 6>::Identity()*dt*dt*max_accel;
  process_noise.bottomRightCorner<6, 6>() =
    Matrix<double, 6, 6>::Identity()*dt*max_accel;
  process_noise *= process_noise; // Make it a covariance
  measurement_noise =
    Matrix<double, 6, 6>::Identity()*1e-3;
  measurement_noise *= measurement_noise; // Make it a covariance
  model_set.insert(model_list.begin(), model_list.end());

  // Connecting to the server
  ROS_INFO_STREAM("Connecting to QTM server at: "
      << server_address << ":" << base_port);

  if(!port_protocol.Connect((char *)server_address.data(), base_port, 0, 1, 7)) {
    ROS_FATAL_STREAM("Connection to QTM server at: "
        << server_address << ":" << base_port << "failed\n"
        "Reason: " << port_protocol.GetErrorString());

    return false;
  }
  ROS_INFO_STREAM("Connected to " << server_address << ":" << base_port);

  // Get 6DOF settings
  bool  bDataAvailable = false;
  port_protocol.Read6DOFSettings(bDataAvailable);
  if (bDataAvailable == false) {
    ROS_FATAL_STREAM("Reading 6DOF body settings failed during intialization\n"
                  << "QTM error: " << port_protocol.GetErrorString());
    return false;
  }
  // Request that the server starts streaming data
  bDataAvailable = port_protocol.StreamFrames(
      CRTProtocol::RateAllFrames,
      0,
      0,
      0,
      CRTProtocol::cComponent6d);

  if (bDataAvailable == false) {
     ROS_FATAL_STREAM("Starting 6DOF frame stream failed during intialization\n"
                   << "QTM error: " << port_protocol.GetErrorString());
    return false;
  }
  // Reserve threads
  // for (int i=0; i<worker_threads; i++){
  //   threadpool.create_thread(
  //     boost::bind(&boost::asio::io_service::run, &ioService)  
  //   );
  // }
  return true;
}

void QualisysDriver::disconnect() {
  ROS_INFO_STREAM("Disconnected from the QTM server at "
      << server_address << ":" << base_port);
  port_protocol.StreamFramesStop();
  port_protocol.Disconnect();
  return;
}

bool QualisysDriver::run() {
  // boost::unique_lock<boost::shared_mutex> write_lock(mtx);
  //ROS_INFO("Getting packet");
  prt_packet = port_protocol.GetRTPacket();
  // write_lock.unlock();
  //ROS_INFO("Packet ot, lock released");
  CRTPacket::EPacketType e_type;
  //port_protocol.GetCurrentFrame(CRTProtocol::Component6d);
  bool is_ok = false;

  if(port_protocol.ReceiveRTPacket(e_type, true)) {
    switch(e_type) {
      // Case 1 - sHeader.nType 0 indicates an error
      case CRTPacket::PacketError:
        ROS_ERROR_STREAM_THROTTLE(
            1, "Error when streaming frames: "
            << port_protocol.GetRTPacket()->GetErrorString());
        break;

      // Case 2 - No more data
      case CRTPacket::PacketNoMoreData:
        ROS_ERROR_STREAM_THROTTLE(1, "No more data, check if RT capture is active");
        break;

      // Case 3 - Data received
      case CRTPacket::PacketData:
        handleFrame();
        is_ok = true;
        break;

      // Case 9 - None type, sent on disconnet
      case CRTPacket::PacketNone:
        break;

      default:
        ROS_WARN_THROTTLE(1, "Unhandled CRTPacket type, case: %i", e_type);
        break;
    }
  }
  return is_ok;
}

void QualisysDriver::handleFrame() {
  // Number of rigid bodies
  int body_count = prt_packet->Get6DOFBodyCount();
  // Compute the timestamp
  unsigned long packet_time = prt_packet->GetTimeStamp();
  if(start_time_local_ == 0)
  {
    start_time_local_ = ros::Time::now().toSec();
    last_packet_time = packet_time;
    start_time_packet_ = last_packet_time / 1e6;
  }
  else
  {
    frame_interval = 0.6*frame_interval + 0.4*(packet_time - last_packet_time)/1e6;
    last_packet_time = packet_time;
  }
  //boost::unique_lock<boost::shared_mutex> write_lock(mtx);
  for (int i = 0; i< body_count; ++i) {
    string subject_name(port_protocol.Get6DOFBodyName(i));

    // Process the subject if required
    if (model_set.empty() || model_set.count(subject_name)) {
      // Create a new subject if it does not exist
      if (subjects.find(subject_name) == subjects.end()) {
        subjects[subject_name] = Subject::SubjectPtr(
            new Subject(&nh, subject_name, fixed_frame_id));
        subjects[subject_name]->setParameters(
            process_noise, measurement_noise, frame_rate);
      }
      // Handle the subject in a worker thread
      //if (worker_threads > 0) {
      //  ioService.post(boost::bind(&QualisysDriver::handleSubject, this, i));
      //} else {
      handleSubject(i);
      //}
    }
  }
  //write_lock.unlock();
  return;
}

void QualisysDriver::handleSubject(int sub_idx) {
  // Name of the subject
  // boost::shared_lock<boost::shared_mutex> read_lock(mtx);
  string subject_name(port_protocol.Get6DOFBodyName(sub_idx));
  // Pose of the subject
  const unsigned int matrix_size = 9;
  float x, y, z;
  float rot_array[matrix_size];
  prt_packet->Get6DOFBody(sub_idx, x, y, z, rot_array);

  // Check if the subject is tracked by looking for NaN in the received data
  bool nan_in_matrix = false;
  for (unsigned int i=0; i < matrix_size; i++){
    if (isnan(rot_array[i])) {
      nan_in_matrix = true;
      break;
    }
  }
  if(isnan(x) || isnan(y) || isnan(z) || nan_in_matrix) {
    if(subjects.at(subject_name)->getStatus() != Subject::LOST){
      subjects.at(subject_name)->disable();
    }
    return;
  }
  
  // Convert the rotation matrix to a quaternion
  Eigen::Matrix<float, 3, 3, Eigen::ColMajor> rot_matrix(rot_array);
  Eigen::Quaterniond m_att(rot_matrix.cast<double>());
  // Check if the subject is beeing tracked

  // Convert mm to m
  Eigen::Vector3d m_pos(x/1000.0, y/1000.0, z/1000.0);
  // Re-enable the object if it is lost previously
  if (subjects.at(subject_name)->getStatus() == Subject::LOST) {
    subjects.at(subject_name)->enable();
  }

  const double packet_time = prt_packet->GetTimeStamp() / 1e6;
  const double time = start_time_local_ + (packet_time - start_time_packet_);

  // Feed the new measurement to the subject
  subjects.at(subject_name)->processNewMeasurement(time, m_att, m_pos);

  // Publish tf if requred
  if (publish_tf &&
      subjects.at(subject_name)->getStatus() == Subject::TRACKED) {

    Quaterniond att = subjects.at(subject_name)->getAttitude();
    Vector3d pos = subjects.at(subject_name)->getPosition();
    tf::Quaternion att_tf;
    tf::Vector3 pos_tf;
    tf::quaternionEigenToTF(att, att_tf);
    tf::vectorEigenToTF(pos, pos_tf);
    ROS_DEBUG("Name: %s,\tindex: %i", subject_name.c_str(), sub_idx);
    ROS_DEBUG("%s rot matrix:\n%f,\t%f,\t%f\n%f,\t%f,\t%f\n%f,\t%f,\t%f\n",
            subject_name.c_str(),
            rot_array[0], rot_array[1], rot_array[2],
            rot_array[3], rot_array[4], rot_array[5],
            rot_array[6], rot_array[7], rot_array[8]);
    ROS_DEBUG("Position\nx: %f,\ty: %f\tz: %f", x/1000.0, y/1000.0, z/1000.0);
    ROS_DEBUG("Quaternion rotation\nx: %f,\ty: %f,\tz: %f,\tw: %f,\t",
            att_tf.getX(), att_tf.getY(), att_tf.getZ(), att_tf.getW());
    tf::StampedTransform stamped_transform =
      tf::StampedTransform(tf::Transform(att_tf, pos_tf),
        ros::Time(time), fixed_frame_id, subject_name);
    tf_publisher.sendTransform(stamped_transform);
  }
  return;
}
}

