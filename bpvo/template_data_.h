#ifndef BPVO_TEMPLATE_DATA__H
#define BPVO_TEMPLATE_DATA__H

#include <bpvo/types.h>
#include <bpvo/debug.h>
#include <bpvo/utils.h>
#include <bpvo/imgproc.h>
#include <bpvo/math_utils.h>

#include <opencv2/core/core.hpp>

#include <utility>
#include <iostream>

#define TEMPLATE_DATA_SET_DATA_WITH_TBB 0
#if TEMPLATE_DATA_SET_DATA_WITH_TBB
#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>
#endif

namespace bpvo {

template <class ChannelsType, class WarpType>
class TemplateData_
{
 public:
  typedef WarpType Warp;
  typedef typename Warp::Point Point;
  typedef typename Warp::Jacobian Jacobian;
  typedef typename Warp::WarpJacobian WarpJacobian;
  typedef typename Warp::PointVector PointVector;
  typedef typename Warp::JacobianVector JacobianVector;
  typedef typename Warp::WarpJacobianVector WarpJacobianVector;

  typedef ChannelsType Channels;
  typedef typename Channels::PixelVector PixelVector;

  static constexpr int NumChannels = Channels::NumChannels;

 public:
  inline TemplateData_(const Matrix33& K, const float& baseline, int pyr_level)
      : _pyr_level(pyr_level), _warp(K, baseline) {}

  inline ~TemplateData_() {}

 public:
  /**
   * set the data, i.e. compute points, pixels and jacobians
   */
  void setData(const Channels& channels, const cv::Mat& disparity);

  void computeResiduals(const Channels& channels, const Matrix44& pose,
                        std::vector<float>& residuals, std::vector<uint8_t>& valid);

  /**
   * reserves memory for 'n' points
   */
  inline void reserve(size_t n)
  {
    _jacobians.reserve(n * NumChannels);
    _points.reserve(n);
    _pixels.reserve(n * NumChannels);
  }

  inline int numPoints() const { return _points.size(); }
  inline int numPixels() const { return _pixels.size(); }

  /**
   * \return the i-th point
   */
  const typename PointVector::value_type& X(size_t i) const { return _points[i]; }
  typename PointVector::value_type& X(size_t i) { return _points[i]; }

  /**
   * \return the i-th jacobian
   */
  const typename JacobianVector::value_type& J(size_t i) const { return _jacobians[i]; }
  typename JacobianVector::value_type& J(size_t i) { return _jacobians[i]; }

  /**
   * \return the i-th pixel value
   */
  const typename PixelVector::value_type& I(size_t i) const { return _pixels[i]; }
  typename PixelVector::value_type& I(size_t i) { return _pixels[i]; }

  inline const Warp warp() const { return _warp; }

 protected:
  int _pyr_level;

  JacobianVector _jacobians;
  PointVector _points;
  PixelVector _pixels;

  Warp _warp;
  //Channels _channels;

  inline void clear()
  {
    _jacobians.clear();
    _points.clear();
    _pixels.clear();
  }


  inline void resize(size_t n)
  {
    _jacobians.resize( n * NumChannels );
    _points.resize( n );
    _pixels.resize( n * NumChannels );
  }

 public:
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW;
}; // TemplateData_

namespace {


template <class SaliencyMapT> std::vector<std::pair<int,float>>
getValidPixelsLocations(const DisparityPyramidLevel& dmap, const SaliencyMapT& smap,
                        int nms_radius, bool do_nonmax_supp)
{
  std::vector<std::pair<int,float>> ret;
  ret.reserve(smap.rows * smap.cols * 0.25);

  const ValidPixelPredicate<SaliencyMapT> is_pixel_valid(dmap, smap, do_nonmax_supp ? nms_radius : -1);
  const int border = std::max(2, nms_radius);
  for(int y = border; y < smap.rows - border - 1; ++y)
    for(int x = border; x < smap.cols - border - 1; ++x) {
      if(is_pixel_valid(y, x)) {
        ret.push_back({y*smap.cols + x, dmap(y,x)});
      }
    }

  return ret;
}

#if TEMPLATE_DATA_SET_DATA_WITH_TBB

template <class TData>
class TemplateDataExtractor
{
 public:
  typedef typename TData::WarpJacobianVector WarpJacobianVector;
  typedef typename TData::Channels Channels;
  typedef Eigen::Matrix<float,1,2> ImageGradient;

 public:
  TemplateDataExtractor(TData& tdata, const WarpJacobianVector& Jw,
                        const std::vector<std::pair<int,float>>& inds,
                        const Channels& channels)
      : _tdata(tdata), _Jw(Jw), _inds(inds), _channels(channels) {}

  inline void operator()(tbb::blocked_range<int>& range) const
  {
    float Fx = _tdata.warp().K()(0,0) * 0.5f;
    float Fy = _tdata.warp().K()(1,1) * 0.5f;
    for(int c = range.begin(); c != range.end(); ++c) {
      auto c_ptr = _channels[c].ptr();
      auto stride = _channels[c].cols;
      auto offset = c * _inds.size();
      auto J_ptr = &_tdata.J(0) + offset;
      auto P_ptr = &_tdata.I(0) + offset;

      for(size_t i = 0; i < _inds.size(); ++i) {
        auto ii = _inds[i].first;
        P_ptr[i] = static_cast<float>(c_ptr[ii]);
        auto Ix = static_cast<float>(c_ptr[ii+1]) - static_cast<float>(c_ptr[ii-1]),
             Iy = static_cast<float>(c_ptr[ii+stride]) - static_cast<float>(c_ptr[ii-stride]);
        J_ptr[i] = ImageGradient(Fx*Ix, Fy*Iy) * _Jw[i];
      }
    }
  }

 protected:
  TData& _tdata;
  const WarpJacobianVector& _Jw;
  const std::vector<std::pair<int,float>>& _inds;
  const Channels& _channels;
}; // TemplateDataExtractor

#endif // TEMPLATE_DATA_SET_DATA_WITH_TBB

}; // namespace

template <class CN, class W> inline
void TemplateData_<CN,W>::setData(const Channels& channels, const cv::Mat& D)
{
  assert( D.type() == cv::DataType<float>::type );


  const auto smap = channels.computeSaliencyMap();
  auto do_nonmax_supp = smap.rows*smap.cols >= AlgorithmParameters::MIN_NUM_FOR_PIXEL_PSELECTION;
  int nms_radius = 1;
  auto inds = getValidPixelsLocations(DisparityPyramidLevel(D, _pyr_level),
                                      smap, nms_radius, do_nonmax_supp);

  //this->clear();
  //this->resize(inds.size());

  _points.resize(inds.size());
  auto stride = smap.cols;

  //
  // compute the points
  //
  for(size_t i = 0; i < inds.size(); ++i) {
    int x = 0, y = 0;
    ind2sub(stride, inds[i].first, y, x);
    _points[i] = _warp.makePoint(x, y, inds[i].second);
  }


  //
  // compute the warp jacobians
  //
  WarpJacobianVector Jw(_points.size());
#pragma omp parallel for
  for(size_t i = 0; i < _points.size(); ++i) {
    Jw[i] = _warp.warpJacobianAtZero(_points[i]);
  }

  _pixels.resize(_points.size() * NumChannels);
  _jacobians.resize(_points.size() * NumChannels);

#if TEMPLATE_DATA_SET_DATA_WITH_TBB
  TemplateDataExtractor<TemplateData_<CN,W>> tde(*this, Jw, inds, channels);
  tbb::parallel_for(tbb::blocked_range<int>(0,NumChannels), tde);
#else
  //
  // compute the pixels and jacobians for all channels
  //
  typedef Eigen::Matrix<float,1,2> ImageGradient;
  auto Fx = _warp.K()(0,0) * 0.5f,
       Fy = _warp.K()(1,1) * 0.5f;

  for(int c = 0; c < channels.size(); ++c) {
    auto c_ptr = channels.channelData(c);
    auto J_ptr = _jacobians.data() + c*inds.size();
    auto P_ptr = _pixels.data() + c*inds.size();

    for(size_t i = 0; i < inds.size(); ++i) {
      auto ii = inds[i].first;
      P_ptr[i] = static_cast<float>(c_ptr[ii]);
      auto Ix = static_cast<float>(c_ptr[ii+1]) - static_cast<float>(c_ptr[ii-1]),
           Iy = static_cast<float>(c_ptr[ii+stride]) - static_cast<float>(c_ptr[ii-stride]);
      J_ptr[i] = ImageGradient(Fx*Ix, Fy*Iy) * Jw[i];
    }
  }
#endif

  // NOTE: we push an empty Jacobian at the end because of SSE code loading
  _jacobians.push_back(Jacobian::Zero());

}

template <class CN, class W> inline
void TemplateData_<CN,W>::computeResiduals(const Channels& channels,
                                           const Matrix44& pose,
                                           std::vector<float>& residuals,
                                           std::vector<uint8_t>& valid)
{
  int max_rows = channels[0].rows-1;
  int max_cols = channels[0].cols-1;
  auto n = numPoints();

  typename EigenAlignedContainer<Eigen::Vector4f>::type interp_coeffs(n);
  typename EigenAlignedContainer<Eigen::Vector2i>::type uv(n);
  valid.resize(n);

  // set the pose for the warp
  _warp.setPose(pose);

  //
  // pre-compute the interpolation coefficients, integer parts and valid flags
  //
  for(int i = 0; i < numPoints(); ++i) {
    auto x = _warp(_points[i]);
#if 1
    int xi = cvFloor(x[0] + 0.5f),
        yi = cvFloor(x[1] + 0.5f);
#else
    int xi = std::floor(x[0] + 0.5f),
        yi = std::floor(x[1] + 0.5f);
#endif
    uv[i] = Eigen::Vector2i(xi, yi);
    float xf = x[0] - xi,
          yf = x[1] - yi;

#if 1
    if(fabs(xf) > 1e-3 || fabs(yf) > 1e-3) {
      std::cout << _points[i].transpose() << std::endl;
      printf("%f %f\n", xf, yf);
      printf("(%f,%f) -> (%d,%d)\n", x[0], x[1], xi, yi);
    }
#endif

    valid[i] = xi >= 0 && xi < max_cols && yi >= 0 && yi < max_rows;
    interp_coeffs[i] = Eigen::Vector4f(
        (1.0f - yf) * (1.0f - xf),
        (1.0f - yf) * xf,
        yf * (1.0f - xf),
        yf * xf);
  }

  // compute the residuals
  residuals.resize(_pixels.size());
  for(int c = 0; c < channels.size(); ++c) {
    auto r_ptr = residuals.data() + c*n; // n points per channel
    auto I0_ptr = _pixels.data() + c*n;
    auto I_ptr = channels.channelData(c);
    auto stride = channels[c].cols;
    for(int i = 0; i < n; ++i) {
      if(valid[i]) {
        auto ii = uv[i].y()*stride + uv[i].x();
        auto Iw = interp_coeffs[i].dot(Eigen::Vector4f(
                I_ptr[ii], I_ptr[ii+1], I_ptr[ii+stride], I_ptr[ii+stride+1]));
        r_ptr[i] = Iw - I0_ptr[i];

        if(fabs(r_ptr[i]) > 1e-3) {
          printf("%f %f\n", Iw, I0_ptr[i]);
        }

      } else {
        r_ptr[i] = 0.0f;
      }
    }
  }

}

#undef TEMPLATE_DATA_SET_DATA_WITH_TBB

}; // bpvo

#endif // BPVO_TEMPLATE_DATA__H