/*
 * Copyright (c) 2008, Willow Garage, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Willow Garage, Inc. nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/* Author: Brian Gerkey */

#define USAGE "\nUSAGE: map_server <map.yaml>\n" \
              "  map.yaml: map description file\n" \
              "DEPRECATED USAGE: map_server <map> <resolution>\n" \
              "  map: image file to load\n"\
              "  resolution: map resolution [meters/pixel]"

#include <stdio.h>
#include <stdlib.h>
#include <libgen.h>
#include <fstream>

#include "ros/ros.h"
#include "ros/console.h"
#include "std_msgs/String.h"
#include "nautonomous_map_server/image_loader.h"
#include "nautonomous_map_msgs/MapLoader.h"
#include "nav_msgs/MapMetaData.h"
#include "yaml-cpp/yaml.h"
#include "std_msgs/Float32MultiArray.h"

#ifdef HAVE_YAMLCPP_GT_0_5_0

// The >> operator disappeared in yaml-cpp 0.5, so this function is
// added to provide support for code written under the yaml-cpp 0.3 API.
template<typename T>
void operator >> (const YAML::Node& node, T& i)
{
  i = node.as<T>();
}
#endif

//Create publisher and service objects
ros::Publisher map_pub;
ros::Publisher metadata_pub;
ros::Publisher map_data;
ros::ServiceServer service;

bool deprecated;

nav_msgs::MapMetaData meta_data_message_;
nav_msgs::GetMap::Response map_resp_;

// Load the map using the service request.
bool load_map(nautonomous_map_msgs::MapLoader::Request &request, nautonomous_map_msgs::MapLoader::Response &response){

    ROS_INFO("Load_map callback");

    std::string image_file_name = request.image_file_name;
    std::string config_file_name = request.config_file_name;

    ROS_INFO("Load_map %s %s", image_file_name.c_str(), config_file_name.c_str());

    double res = 0.0;
    double origin[3];
    double gps_origin_data[2];
    std_msgs::Float32MultiArray gps_origin;
    int negate;
    double occ_th, free_th;
    MapMode mode = TRINARY;
    std::string frame_id;
    ros::NodeHandle private_nh("~");
    private_nh.param("frame_id", frame_id, std::string("map"));

    std::ifstream fin(config_file_name.c_str());
    if (fin.fail()) {
      ROS_ERROR("Map_server could not open %s.", config_file_name.c_str());
      exit(-1);
    }
#ifdef HAVE_YAMLCPP_GT_0_5_0
    // The document loading process changed in yaml-cpp 0.5.
    YAML::Node doc = YAML::Load(fin);
#else
    YAML::Parser parser(fin);
    YAML::Node doc;
    parser.GetNextDocument(doc);
#endif
    try {
      doc["resolution"] >> res;
    } catch (YAML::InvalidScalar) {
      ROS_ERROR("The map does not contain a resolution tag or it is invalid.");
      exit(-1);
    }
    try {
      doc["negate"] >> negate;
    } catch (YAML::InvalidScalar) {
      ROS_ERROR("The map does not contain a negate tag or it is invalid.");
      exit(-1);
    }
    try {
      doc["occupied_thresh"] >> occ_th;
    } catch (YAML::InvalidScalar) {
      ROS_ERROR("The map does not contain an occupied_thresh tag or it is invalid.");
      exit(-1);
    }
    try {
      doc["free_thresh"] >> free_th;
    } catch (YAML::InvalidScalar) {
      ROS_ERROR("The map does not contain a free_thresh tag or it is invalid.");
      exit(-1);
    }
    try {
      std::string modeS = "";
      doc["mode"] >> modeS;

      if(modeS=="trinary")
        mode = TRINARY;
      else if(modeS=="scale")
        mode = SCALE;
      else if(modeS=="raw")
        mode = RAW;
      else{
        ROS_ERROR("Invalid mode tag \"%s\".", modeS.c_str());
        exit(-1);
      }
    } catch (YAML::Exception) {
      ROS_DEBUG("The map does not contain a mode tag or it is invalid... assuming Trinary");
      mode = TRINARY;
    }
    try {
      doc["origin"][0] >> origin[0];
      doc["origin"][1] >> origin[1];
      doc["origin"][2] >> origin[2];
    } catch (YAML::InvalidScalar) {
      ROS_ERROR("The map does not contain an origin tag or it is invalid.");
      exit(-1);
    }
    try {
      //doc["image"] >> mapfname;
      // TODO: make this path-handling more robust
      if(image_file_name.size() == 0)
      {
        ROS_ERROR("The image tag cannot be an empty string.");
        exit(-1);
      }
      if(image_file_name[0] != '/')
      {
        // dirname can modify what you pass it
        char* fname_copy = strdup(config_file_name.c_str());
        image_file_name = std::string(dirname(fname_copy)) + '/' + image_file_name;
        free(fname_copy);
      }
    } catch (YAML::InvalidScalar) {
      ROS_ERROR("The map does not contain an image tag or it is invalid.");
      exit(-1);
    }

    ROS_INFO("Loading map from image \"%s\"", image_file_name.c_str());
    nautonomous_map_server::loadMapFromFile(&map_resp_,image_file_name.c_str(),res,negate,occ_th,free_th, origin, mode);
    map_resp_.map.info.map_load_time = ros::Time::now();
    map_resp_.map.header.frame_id = frame_id;
    map_resp_.map.header.stamp = ros::Time::now();
    ROS_INFO("Read a %d X %d map @ %.3lf m/cell",
              map_resp_.map.info.width,
              map_resp_.map.info.height,
              map_resp_.map.info.resolution);
    meta_data_message_ = map_resp_.map.info;


    // Latched publisher for metadata
    metadata_pub.publish( meta_data_message_ );

    // Latched publisher for data
    map_pub.publish( map_resp_.map );

    // Latched publisher for map data (gps center)
    //map_data.publish(gps_origin);
    ROS_INFO("New map data published");

    response.status = "ok";

    return true;
}


int main(int argc, char **argv)
{

  ros::init(argc, argv, "nautonomous_map_server");
  ros::NodeHandle n;

  map_data = n.advertise<std_msgs::Float32MultiArray>("/map/server/map_data", 1, true);      

  metadata_pub= n.advertise<nav_msgs::MapMetaData>("/map/server/map_metadata", 1, true);
  map_pub = n.advertise<nav_msgs::OccupancyGrid>("/map/server/map", 1, true);
  
  ros::ServiceServer service = n.advertiseService("/map/server/load_map", load_map);
  ROS_INFO("Initialized service");
  
  ros::spin();

  return 0;
}

