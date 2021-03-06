﻿#include "salsa/salsa.h"

using namespace std;
using namespace Eigen;
using namespace xform;
using namespace quat;

namespace salsa
{

bool Salsa::isTrackedFeature(int id) const
{
    FeatMap::const_iterator it = xfeat_.begin();
    while (it != xfeat_.end() && id != it->first)
        ++it;
    return it != xfeat_.end();
}

void Salsa::imageCallback(const double& tc, const ImageFeat& z,
                          const Matrix2d& R_pix, const Matrix1d& R_depth)
{
    SD(3, "IMG CB t%.3f", tc);
    if (disable_vision_)
        return;

    if (enable_static_start_ && (lt(tc, static_start_end_ + camera_start_delay_)))
    {
        SD(2, "Waiting for Camera delay after static start");
        return;
    }


    if (sim_KLT_)
    {
        SD(1, "Simulating KLT");
        if (current_img_.empty())
        {
            current_img_.create(cv::Size(cam_.image_size_(0), cam_.image_size_(1)), 0);
        }
        current_img_ = 0;
        for (auto pix : z.pixs)
        {
            int d = 2;
            cv::Point2d xp(pix(0)+d, pix(1));
            cv::Point2d xm(pix(0)-d, pix(1));
            cv::Point2d yp(pix(0), pix(1)+d);
            cv::Point2d ym(pix(0), pix(1)-d);
            cv::line(current_img_, xp, xm, 255);
            cv::line(current_img_, yp, ym, 255);
        }
        imageCallback(tc, current_img_, R_pix);
    }
    else
    {
        Features zfeat;
        zfeat.id = z.id;
        zfeat.t = z.t;
        zfeat.feat_ids = z.feat_ids;
        zfeat.depths = z.depths;
        zfeat.zetas.reserve(z.pixs.size());
        zfeat.pix.reserve(z.pixs.size());
        for (auto pix : z.pixs)
        {
            zfeat.zetas.emplace_back(cam_.invProj(pix, 1.0));
            zfeat.pix.emplace_back(pix.x(), pix.y());
        }
        bool new_keyframe = calcNewKeyframeCondition(zfeat);
        if (!disable_vision_)
            addMeas(meas::Img(tc, zfeat, R_pix, new_keyframe));
    }
}

void Salsa::initializeStateImage(const meas::Img &m)
{
    initialize(m.t, x0_, v0_, Vector2d::Zero());
}

void Salsa::imageUpdate(const meas::Img &m, int idx)
{
    // Sanity Checks
    SALSA_ASSERT(xbuf_[idx].kf < 0, "Cannot add measuremnts to keyframe");
    SALSA_ASSERT(lastKfId() > 0 ? (xbuf_[idx].node > xbuf_[lastKfId()].node) : true,
                 "Cannot add image measurements before keyframe");


    for (auto& ft : xfeat_)
        ft.second.updated_in_last_image_ = false;

    SD(2, "Image Update, t=%.3f", m.t);
    for (int i = 0; i < m.z.zetas.size(); i++)
    {
        if (isTrackedFeature(m.z.feat_ids[i]))
        {
            Feat& ft(xfeat_.at(m.z.feat_ids[i]));
            if (ft.funcs.size() == 0 || (xbuf_[ft.funcs.back().to_idx_].kf >= 0))
            {
                SD(1, "Adding new measurement to feature %d", m.z.feat_ids[i]);
                ft.addMeas(m.t, idx, m.R_pix, x_b2c_, m.z.zetas[i], Vector2d(m.z.pix[i].x, m.z.pix[i].y));
            }
            else
            {
                SD(1, "Moving feature measurement %d", m.z.feat_ids[i]);
                ft.moveMeas(m.t, idx, m.z.zetas[i]);
            }
            ft.updated_in_last_image_ = true;
            ft.funcs.back().rho_true_ = 1.0/m.z.depths[i];
        }
        else if (m.new_keyframe)
        {
            double rho0 = 1.0/max_depth_;
            if (use_measured_depth_ && !std::isnan(m.z.depths[i]))
                rho0 = 1.0/m.z.depths[i];
            SD(1, "Adding new feature %d", m.z.feat_ids[i]);
            xfeat_.insert({m.z.feat_ids[i], Feat(m.t, xbuf_head_, current_kf_+1, m.z.zetas[i], Vector2d(m.z.pix[i].x, m.z.pix[i].y), rho0, 1.0/m.z.depths[i])});
        }
    }
    xbuf_[idx].type |= State::Camera;
    xbuf_[idx].n_cam++;;

    if (m.new_keyframe)
    {
        setNewKeyframe(idx);
        SD(2, "New Keyframe %d", current_kf_);
    }
    rmLostFeatFromKf();
}

bool Salsa::calcNewKeyframeCondition(const Features &z)
{
    if (current_kf_ == -1)
    {
        kf_condition_ = FIRST_KEYFRAME;
        kf_feat_ = z;
        kf_num_feat_ = z.feat_ids.size();
        SD(2, "new keyframe, first image");
        return true;
    }

    kf_parallax_ = 0;
    kf_Nmatch_feat_ = 0;
    Quatd q_I2i(lastKfState().x.q());
    Quatd q_I2j(current_state_.x.q());
    Quatd q_b2c(x_b2c_.q());
    Quatd q_cj2ci = q_b2c.inverse() * q_I2j.inverse() * q_I2i * q_b2c;
    Matrix3d R_cj2ci = q_cj2ci.R();
    int ni = 0;
    int nj = 0;
    while (ni < kf_feat_.zetas.size() && nj < z.zetas.size())
    {
        int idi = kf_feat_.feat_ids[ni];
        int idj = z.feat_ids[nj];
        if (idi == idj)
        {
            Vector3d zihat = R_cj2ci * z.zetas[nj];
            Vector2d lihat =  cam_.proj(zihat);
            Vector2d li(kf_feat_.pix[ni].x, kf_feat_.pix[ni].y);
            double err = (lihat - li).norm();
            SD(1, "pix error %.2f. lihat = (%f,%f), li = (%f,%f)", err, lihat.x(), lihat.y(), li.x(), li.y());
            kf_parallax_ += err;
            ni++;
            nj++;
            kf_Nmatch_feat_++;
        }
        else if (idi < idj)
            ni++;
        else if (idi > idj)
            nj++;
    }
    kf_parallax_ /= kf_Nmatch_feat_;

    if (kf_parallax_ > kf_parallax_thresh_)
    {
        kf_condition_ = TOO_MUCH_PARALLAX;
        kf_feat_ = z;
        kf_num_feat_ = z.feat_ids.size();
        SD(2, "new keyframe, too much parallax: = %f, nmatch = %d", kf_parallax_, kf_Nmatch_feat_);
        return true;
    }
    else if(kf_Nmatch_feat_ <= std::round(kf_feature_thresh_ * kf_num_feat_) + 0.001)
    {
        kf_condition_ = INSUFFICIENT_MATCHES;
        kf_feat_ = z;
        kf_num_feat_ = z.feat_ids.size();
        SD(2, "new keyframe, not enough matches: = %d/%f",
           kf_Nmatch_feat_, std::round(kf_feature_thresh_ * kf_num_feat_));
        return true;
    }
    else
        return false;
}

void Salsa::cleanUpFeatureTracking(int oldest_kf_idx)
{
    FeatMap::iterator fit = xfeat_.begin();
    while (fit != xfeat_.end())
    {
        SD(1, "Attempting to Slide anchor for Feature %d, %d->%d", fit->first, fit->second.idx0, oldest_kf_idx);
        if (!fit->second.slideAnchor(oldest_kf_idx, xbuf_, x_b2c_))
        {
            SD(1, "Unable to slide, removing feature %d", fit->first);
            fit = xfeat_.erase(fit);
        }
        else
            fit++;
    }
    SALSA_ASSERT(checkFeatures(), "Features Lost Wiring");
}

void Salsa::createNewKeyframe()
{
    filterFeaturesTooClose(track_feature_min_distance_);
    collectNewfeatures();
    kf_feat_ = current_feat_;
    kf_num_feat_ = kf_feat_.size();
    SD(2, "Creating new Keyframe with %d features", kf_feat_.size());
    current_img_.copyTo(kf_img_);
}

void Salsa::rmLostFeatFromKf()
{
    FeatMap::iterator ftpair = xfeat_.begin();
    while (ftpair != xfeat_.end())
    {
        Feat& ft(ftpair->second);
        if (ft.funcs.size() == 0)
        {
            ftpair++;
            continue;
        }
        else if (!ft.updated_in_last_image_
                 && (ft.funcs.back().to_idx_ == xbuf_head_ || xbuf_[ft.funcs.back().to_idx_].kf < 0))
        {
            SD(1, "Feature %d not tracked", ftpair->first);
            ft.funcs.pop_back();
            if (ft.funcs.size() == 0)
            {
                SD(2, "removing feature %d", ftpair->first);
                ftpair = xfeat_.erase(ftpair);
                continue;
            }
        }
        ftpair++;
    }
}

int Salsa::numTotalFeat() const
{
    int n_feat_meas = 0;
    for (auto& ft : xfeat_)
    {
        n_feat_meas += 1 + ft.second.funcs.size();
    }
    return n_feat_meas;
}

void Salsa::fixDepth()
{
    for (auto& ft : xfeat_)
    {
        if (ft.second.rho < 1.0/max_depth_)
        {
            ft.second.rho = 1.0/(2.0 * max_depth_);
        }
    }
}



}
