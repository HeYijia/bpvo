### BPVO

A library for (semi-dense) real-time visual odometry.


USAGE

For examples see ./apps/vo.cc


A minimal example is as follows:

  include <bpvo/vo.h>

  using namespace bpvo;

  int main() {

	  // setup phase
	  // Get the camera intrinsics and baseline
	  Matrix33 K; // this is just an Eigen::Matrix<float,3,3>
	  float b;

	  // you also need the imgae size
	  ImageSize image_size = ...

	  // decide on the parameters you want to use
	  AlgorithmParameters params = ...

	  // create vo
	  VisualOdometry vo(K, b, image_size, params);

	  // keep adding frames
	  while(have_data) {
		auto result = vo.addFrame(image_ptr, disparity_ptr);
	  }

  }

