// Copyright 2014 BVLC and contributors.

#include <stdint.h>
#include <fcntl.h>
#include <google/protobuf/text_format.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/io/coded_stream.h>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/highgui/highgui_c.h>
#include <opencv2/imgproc/imgproc.hpp>

#include <algorithm>
#include <string>
#include <vector>
#include <fstream>  // NOLINT(readability/streams)
#include <exception>

#include "caffe/common.hpp"
#include "caffe/util/io.hpp"
#include "caffe/proto/caffe.pb.h"

using std::fstream;
using std::ios;
using std::max;
using std::string;
using google::protobuf::io::FileInputStream;
using google::protobuf::io::FileOutputStream;
using google::protobuf::io::ZeroCopyInputStream;
using google::protobuf::io::CodedInputStream;
using google::protobuf::io::ZeroCopyOutputStream;
using google::protobuf::io::CodedOutputStream;
using google::protobuf::Message;

namespace caffe {

  bool ReadProtoFromTextFile(const char* filename, Message* proto) {
    int fd = open(filename, O_RDONLY);
    CHECK_NE(fd, -1) << "File not found: " << filename;
    FileInputStream* input = new FileInputStream(fd);
    bool success = google::protobuf::TextFormat::Parse(input, proto);
    delete input;
    close(fd);
    return success;
  }

  void WriteProtoToTextFile(const Message& proto, const char* filename) {
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    FileOutputStream* output = new FileOutputStream(fd);
    CHECK(google::protobuf::TextFormat::Print(proto, output));
    delete output;
    close(fd);
  }

  bool ReadProtoFromBinaryFile(const char* filename, Message* proto) {
    int fd = open(filename, O_RDONLY);
    CHECK_NE(fd, -1) << "File not found: " << filename;
    ZeroCopyInputStream* raw_input = new FileInputStream(fd);
    CodedInputStream* coded_input = new CodedInputStream(raw_input);
    coded_input->SetTotalBytesLimit(1073741824, 536870912);

    bool success = proto->ParseFromCodedStream(coded_input);

    delete coded_input;
    delete raw_input;
    close(fd);
    return success;
  }

  void WriteProtoToBinaryFile(const Message& proto, const char* filename) {
    fstream output(filename, ios::out | ios::trunc | ios::binary);
    CHECK(proto.SerializeToOstream(&output));
  }

  bool ReadImageToDatum(const string& filename, const int label,
			const int height, const int width, Datum* datum) {
    cv::Mat cv_img;
    if (height > 0 && width > 0) {
      cv::Mat cv_img_origin = cv::imread(filename, CV_LOAD_IMAGE_COLOR);
      cv::resize(cv_img_origin, cv_img, cv::Size(height, width));
    } else {
      cv_img = cv::imread(filename, CV_LOAD_IMAGE_COLOR);
    }
    if (!cv_img.data) {
      LOG(ERROR) << "Could not open or find file " << filename;
      return false;
    }
    datum->set_channels(3);
    datum->set_height(cv_img.rows);
    datum->set_width(cv_img.cols);
    datum->set_label(label);
    datum->clear_data();
    datum->clear_float_data();
    string* datum_string = datum->mutable_data();
    for (int c = 0; c < 3; ++c) {
      for (int h = 0; h < cv_img.rows; ++h) {
	for (int w = 0; w < cv_img.cols; ++w) {
	  datum_string->push_back(
				  static_cast<char>(cv_img.at<cv::Vec3b>(h, w)[c]));
	}
      }
    }
    return true;
  }

  // Verifies format of data stored in HDF5 file and reshapes blob accordingly.
  template <typename Dtype>
  void hdf5_load_nd_dataset_helper(
				   hid_t file_id, const char* dataset_name_, int min_dim, int max_dim,
				   Blob<Dtype>* blob) {
    // Verify that the number of dimensions is in the accepted range.
    herr_t status;
    int ndims;
    status = H5LTget_dataset_ndims(file_id, dataset_name_, &ndims);
    CHECK_GE(ndims, min_dim);
    CHECK_LE(ndims, max_dim);

    // Verify that the data format is what we expect: float or double.
    std::vector<hsize_t> dims(ndims);
    H5T_class_t class_;
    status = H5LTget_dataset_info(
				  file_id, dataset_name_, dims.data(), &class_, NULL);
    CHECK_EQ(class_, H5T_FLOAT) << "Expected float or double data";

    blob->Reshape(
		  dims[0],
		  (dims.size() > 1) ? dims[1] : 1,
		  (dims.size() > 2) ? dims[2] : 1,
		  (dims.size() > 3) ? dims[3] : 1);
  }

  template <>
  void hdf5_load_nd_dataset<float>(hid_t file_id, const char* dataset_name_,
				   int min_dim, int max_dim, Blob<float>* blob) {
    hdf5_load_nd_dataset_helper(file_id, dataset_name_, min_dim, max_dim, blob);
    herr_t status = H5LTread_dataset_float(
					   file_id, dataset_name_, blob->mutable_cpu_data());
  }

  template <>
  void hdf5_load_nd_dataset<double>(hid_t file_id, const char* dataset_name_,
				    int min_dim, int max_dim, Blob<double>* blob) {
    hdf5_load_nd_dataset_helper(file_id, dataset_name_, min_dim, max_dim, blob);
    herr_t status = H5LTread_dataset_double(
					    file_id, dataset_name_, blob->mutable_cpu_data());
  }

  template <>
  void hdf5_save_nd_dataset<float>(
				   const hid_t file_id, const string dataset_name, const Blob<float>& blob) {
    hsize_t dims[HDF5_NUM_DIMS];
    dims[0] = blob.num();
    dims[1] = blob.channels();
    dims[2] = blob.height();
    dims[3] = blob.width();
    herr_t status = H5LTmake_dataset_float(
					   file_id, dataset_name.c_str(), HDF5_NUM_DIMS, dims, blob.cpu_data());
    CHECK_GE(status, 0) << "Failed to make float dataset " << dataset_name;
  }

  template <>
  void hdf5_save_nd_dataset<double>(
				    const hid_t file_id, const string dataset_name, const Blob<double>& blob) {
    hsize_t dims[HDF5_NUM_DIMS];
    dims[0] = blob.num();
    dims[1] = blob.channels();
    dims[2] = blob.height();
    dims[3] = blob.width();
    herr_t status = H5LTmake_dataset_double(
					    file_id, dataset_name.c_str(), HDF5_NUM_DIMS, dims, blob.cpu_data());
    CHECK_GE(status, 0) << "Failed to make double dataset " << dataset_name;
  }

  template<>
  void WriteDataToImage<float>(string filename, const int channel, const int height, const int width, float* data){
    //The pixels stored in data is organized as: channel, height, and width
    //In opencv::Mat, the data is stored as col (width)->row(height)->channel
    //So if the data is 3-channel, need to re-organized the data
    cv::Mat* img;
    if (channel == 3){
      float* imgdata = new float[channel * width * height];
      int data_offset = height * width;
      for(int r = 0; r<height; r++){
        for(int c = 0; c<width; c++){
	  for(int i = 0; i<3; i++){
	    int pixel_offset = r * width + c;
	    imgdata[pixel_offset * 3 + i] = data[pixel_offset + data_offset * i];
	  }
        }
      }
      img = new cv::Mat(height, width, CV_32FC3, imgdata);
    }
    else if( channel == 1){
      img = (new cv::Mat(height, width, CV_32F, data));
    }
    cv::imwrite(filename, *img);
  }

  template<>
  void WriteDataToImage<double>(string filename, const int channel, const int height, const int width, double* data){
    //The pixels stored in data is organized as: channel, height, and width
    //In opencv::Mat, the data is stored as col (width)->row(height)->channel
    //So if the data is 3-channel, need to re-organized the data
    cv::Mat* img;
    if (channel == 3){
      double* imgdata = new double[channel * width * height];
      int data_offset = height * width;
      for(int r = 0; r<height; r++){
	for(int c = 0; c<width; c++){
	  for(int i = 0; i<3; i++){
	    int pixel_offset = r * width + c;
	    imgdata[pixel_offset * 3 + i] = data[pixel_offset + data_offset * i];
	  }
	}
      }
      img = new cv::Mat(height, width, CV_64FC3, imgdata);
    }
    else if( channel == 1){
      img = (new cv::Mat(height, width, CV_64F, data));
    }
    cv::imwrite(filename, *img);
  }

  template<>
  void ImageNormalization<float>(float* imgData, int len, float mean, float scaler){
    float max = (float)0.;
    float min = imgData[0];
    for (int i = 0; i<len; i++){
      //imgData[i] = fabs(imgData[i]);
      float val = imgData[i];
      if(val < min) min = val;
      if(val > max) max = val;
    }
    LOG(INFO)<<"Max / Min: " << max <<" / "<<min;
    //Normalization
    for(int i = 0; i<len; i++) {
      if(scaler == 1.0){
	if (min == max) {
	  //the situation of all 0's
	  imgData[i] = mean;
	}
	else{
	  //imgData[i] = std::max(imgData[i] / max * (255 - mean) + mean, mean);
	  imgData[i] = imgData[i] / max * (255 - mean) + mean;
	}
      }
      else if(scaler > 0){
	imgData[i] = imgData[i] * scaler + mean;
      }
      //imgData[i] = std::max(mean, imgData[i]);
    }
  }

  template<>
  void ImageNormalization<double>(double* imgData, int len, double mean, double scaler){
    double max = (double)0.;
    double min = imgData[0];
    for (int i = 0; i<len; i++){
      //imgData[i] = fabs(imgData[i]);
      double val = imgData[i];
      if(val < min) min = val;
      if(val > max) max = val;
    }
    //Normalization
    LOG(INFO)<<"Max / Min: " << max <<" / "<<min;
    for(int i = 0; i<len; i++) {
      if(scaler == 1.0){
	if (min == max) {
	  //the situation of all 0's
	  imgData[i] = mean;
	}
	else{
	  //imgData[i] = std::max(imgData[i] / max * (255 - mean) + mean, mean);
	  imgData[i] = imgData[i] / max * (255 - mean) + mean;
	}
      }
      else if(scaler > 0){
	imgData[i] = imgData[i] * scaler + mean;
      }
      //imgData[i] = std::max(mean, imgData[i]);
    }
  }

  //Write the data in blobs into text files named in filename
  template<>
  void writeDataToTxt<float>(string filename, Blob<float>* data){
    try{
      std::ofstream fn(filename.c_str(), std::ofstream::out);
      const float* data_ptr = data->cpu_data();
      for(int n = 0; n<data->num(); ++n){
	for(int c = 0; c<data->channels(); ++c){
	  for(int h = 0; h<data->height(); ++h){
	    for(int w = 0; w<data->width(); ++w){
	      fn<<*(data_ptr + h * data->width() + w)<<" ";
	    }//each line
	    fn<<"\n";
	  }//each column
	  data_ptr += data->offset(0,1);
	}//each channel
      }//each image
      fn.close();
    }//try
    catch(int e){
      LOG(ERROR)<<"Failed to open file";
    }
  }

  template<>
  void writeDataToTxt<double>(string filename, Blob<double>* data){
    try{
      std::ofstream fn(filename.c_str(), std::ofstream::out);
      const double* data_ptr = data->cpu_data();
      for(int n = 0; n<data->num(); ++n){
	for(int c = 0; c<data->channels(); ++c){
	  for(int h = 0; h<data->height(); ++h){
	    for(int w = 0; w<data->width(); ++w){
	      fn<<*(data_ptr + h * data->width() + w)<<" ";
	    }
	    fn<<"\n";
	  }
	  data_ptr += data->offset(0,1);
	}
      }
      fn.close();
    }
    catch (int e){
      LOG(ERROR)<<"Failed to open file";
    }
  }

}  // namespace caffe
