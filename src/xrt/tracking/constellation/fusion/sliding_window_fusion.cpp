// Copyright 2026, Beyley Cardellio
// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Experimental IMU + optical sliding-window fusion (offline evaluation).
 *
 * The factor graph, the first-estimate-Jacobian wrapper, the marginalisation prior factor and the eviction procedure
 * are adapted from Monado MR 3015's `optimizer/sensor_fusion.cpp` (head cd7174257, Beyley Cardellio). The window
 * bookkeeping is rewritten for a synchronous, single-device, moving-camera replay; see the header for the list of
 * changes and additions.
 *
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @author Nick Kennedy
 * @ingroup tracking
 */

#include "sliding_window_fusion.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <map>
#include <set>

namespace xrt::tracking::constellation::fusion {

namespace {

	typedef Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> RowMajorMatrixXd;

	constexpr int kPoseSize = PoseWithVelocity<double>::kNumParameters;
	constexpr int kBiasSize = ImuBias<double>::kNumParameters;
	constexpr int kExtrinsicsSize = ImuExtrinsics<double>::kNumParameters;
	constexpr int kGravitySize = WorldGravity<double>::kNumParameters;

	//! Velocity seeds above this are a bad seed rather than a fast device (upstream: 10 m/s).
	constexpr double kMaxSeedVelocityMetersPerSecond = 10.0;

	/*
	 *
	 * Factors (adapted from upstream sensor_fusion.cpp).
	 *
	 */

	/*!
	 * One camera's view of the device at one keyframe. Upstream's StaticCameraObservationCostFunctor: the camera
	 * pose is a constant of the factor, which is exactly what lets it move between keyframes.
	 */
	struct CameraObservationCostFunctor
	{
		t_camera_model_params params;
		std::vector<Eigen::Vector2d> points2d;
		std::vector<Eigen::Vector3d> points3d;
		Eigen::Isometry3d Tcv_cam_world;
		double sigma_px;

		template <typename T>
		bool
		operator()(const T *const keyframe_parameters,
		           const T *const imu_extrinsics_parameters,
		           T *residuals) const
		{
			const Pose<T> T_world_device{
			    Map<const Vector<T, Pose<T>::kNumParameters>>(keyframe_parameters)};
			const ImuExtrinsics<T> extrinsics{
			    Map<const Vector<T, ImuExtrinsics<T>::kNumParameters>>(imu_extrinsics_parameters)};

			const Eigen::Transform<T, 3, Eigen::Isometry> T_cam_imu =
			    Tcv_cam_world.cast<T>() * T_world_device.toTransform();
			const Quaternion<T> Q_cam_model = Quaternion<T>(T_cam_imu.rotation()) * extrinsics.Q_imu_model;
			const Vector3<T> T_cam_model = T_cam_imu.translation();

			for (size_t i = 0; i < points2d.size(); i++) {
				T *r = residuals + i * kNumLedResiduals;
				computeLedResidual<T>(params, T_cam_model, Q_cam_model, points2d[i].cast<T>(),
				                      points3d[i].cast<T>(), r);
				r[0] /= T(sigma_px);
				r[1] /= T(sigma_px);
			}
			return true;
		}
	};

	typedef ceres::AutoDiffCostFunction<CameraObservationCostFunctor, ceres::DYNAMIC, kPoseSize, kExtrinsicsSize>
	    CameraObservationCostFunction;

	/*!
	 * Optional: the LED model orientation (body orientation times the IMU extrinsic) near the frontend's, whose
	 * tilt M1 regularises with the driver's IMU orientation. Not in upstream.
	 */
	struct SeedOrientationCostFunctor
	{
		Eigen::Quaterniond q_seed;
		double sigma_rad;

		template <typename T>
		bool
		operator()(const T *const keyframe_parameters,
		           const T *const imu_extrinsics_parameters,
		           T *residuals) const
		{
			const Pose<T> T_world_device{
			    Map<const Vector<T, Pose<T>::kNumParameters>>(keyframe_parameters)};
			const ImuExtrinsics<T> extrinsics{
			    Map<const Vector<T, ImuExtrinsics<T>::kNumParameters>>(imu_extrinsics_parameters)};
			Quaternion<T> error =
			    q_seed.conjugate().cast<T>() * (T_world_device.rotation * extrinsics.Q_imu_model);
			if (error.w() < T(0)) {
				error.coeffs() = -error.coeffs();
			}
			Map<Vector3<T>> out(residuals);
			out = quat_ln_so3(error) / T(sigma_rad);
			return true;
		}
	};

	typedef ceres::AutoDiffCostFunction<SeedOrientationCostFunctor, 3, kPoseSize, kExtrinsicsSize>
	    SeedOrientationCostFunction;

	/*!
	 * Evaluates residuals where the states are, Jacobians at their first estimates. Upstream's
	 * FirstEstimateJacobianCostFunction, unchanged.
	 */
	class FirstEstimateJacobianCostFunction final : public ceres::CostFunction
	{
	public:
		static constexpr size_t kMaxParameterBlocks = 8;
		static constexpr int kMaxAmbientSize = 10;

		struct Anchor
		{
			const double *linearization_point{nullptr};
			const ceres::Manifold *manifold{nullptr};
		};

		FirstEstimateJacobianCostFunction(ceres::CostFunction *inner, std::vector<Anchor> anchors)
		    : inner(inner), anchors(std::move(anchors))
		{
			assert(this->anchors.size() == this->inner->parameter_block_sizes().size());
			this->set_num_residuals(this->inner->num_residuals());
			*this->mutable_parameter_block_sizes() = this->inner->parameter_block_sizes();
		}

		bool
		Evaluate(double const *const *parameters, double *residuals, double **jacobians) const override
		{
			if (jacobians == nullptr) {
				return this->inner->Evaluate(parameters, residuals, nullptr);
			}

			std::array<const double *, kMaxParameterBlocks> anchored{};
			for (size_t i = 0; i < this->anchors.size(); i++) {
				anchored[i] = this->anchors[i].linearization_point != nullptr
				                  ? this->anchors[i].linearization_point
				                  : parameters[i];
			}
			if (!this->inner->Evaluate(anchored.data(), residuals, jacobians)) {
				return false;
			}

			for (size_t i = 0; i < this->anchors.size(); i++) {
				const Anchor &anchor = this->anchors[i];
				if (anchor.linearization_point == nullptr || anchor.manifold == nullptr ||
				    jacobians[i] == nullptr) {
					continue;
				}
				// Lift through the anchor's plus Jacobian and down through the current point's minus
				// Jacobian, so Ceres ends up holding the tangent Jacobian at the anchor.
				const int ambient_size = anchor.manifold->AmbientSize();
				const int tangent_size = anchor.manifold->TangentSize();
				SmallRowMajorMatrix plus(ambient_size, tangent_size);
				SmallRowMajorMatrix minus(tangent_size, ambient_size);
				if (!anchor.manifold->PlusJacobian(anchor.linearization_point, plus.data()) ||
				    !anchor.manifold->MinusJacobian(parameters[i], minus.data())) {
					return false;
				}
				Map<RowMajorMatrixXd> jacobian(jacobians[i], this->num_residuals(), ambient_size);
				jacobian = jacobian * (plus * minus);
			}

			return this->inner->Evaluate(parameters, residuals, nullptr);
		}

	private:
		typedef Eigen::
		    Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor, kMaxAmbientSize, kMaxAmbientSize>
		        SmallRowMajorMatrix;

		std::unique_ptr<ceres::CostFunction> inner;
		std::vector<Anchor> anchors;
	};

	/*!
	 * The linear factor a marginalisation leaves behind, |J* dx + r*|^2, with the Jacobian held at the
	 * linearisation point. Upstream's MarginalizationPriorCostFunction, unchanged.
	 */
	class MarginalizationPriorCostFunction final : public ceres::CostFunction
	{
	public:
		struct Block
		{
			std::vector<double> linearization_point{};
			const ceres::Manifold *manifold{nullptr};
		};

		MarginalizationPriorCostFunction(MatrixXd j, VectorXd e0, std::vector<Block> blocks)
		    : j(std::move(j)), e0(std::move(e0)), blocks(std::move(blocks))
		{
			this->set_num_residuals(static_cast<int>(this->e0.rows()));
			for (const Block &block : this->blocks) {
				this->mutable_parameter_block_sizes()->push_back(
				    static_cast<int>(block.linearization_point.size()));
			}
		}

		bool
		Evaluate(double const *const *parameters, double *residuals, double **jacobians) const override
		{
			VectorXd delta_chi(this->j.cols());
			int offset = 0;
			for (size_t i = 0; i < this->blocks.size(); i++) {
				const Block &block = this->blocks[i];
				const int tangent = tangentSize(block);
				if (block.manifold != nullptr) {
					if (!block.manifold->Minus(parameters[i], block.linearization_point.data(),
					                           delta_chi.data() + offset)) {
						return false;
					}
				} else {
					delta_chi.segment(offset, tangent) =
					    Map<const VectorXd>(parameters[i], tangent) -
					    Map<const VectorXd>(block.linearization_point.data(), tangent);
				}
				offset += tangent;
			}

			Map<VectorXd>(residuals, this->num_residuals()) = this->e0 + (this->j * delta_chi);

			if (jacobians == nullptr) {
				return true;
			}

			offset = 0;
			for (size_t i = 0; i < this->blocks.size(); i++) {
				const Block &block = this->blocks[i];
				const int tangent = tangentSize(block);
				const int ambient = static_cast<int>(block.linearization_point.size());
				const int block_offset = offset;
				offset += tangent;
				if (jacobians[i] == nullptr) {
					continue;
				}
				Map<RowMajorMatrixXd> block_jacobian(jacobians[i], this->num_residuals(), ambient);
				if (block.manifold == nullptr) {
					block_jacobian = this->j.middleCols(block_offset, tangent);
					continue;
				}
				RowMajorMatrixXd minus_jacobian(tangent, ambient);
				if (!block.manifold->MinusJacobian(parameters[i], minus_jacobian.data())) {
					return false;
				}
				block_jacobian = this->j.middleCols(block_offset, tangent) * minus_jacobian;
			}
			return true;
		}

	private:
		static int
		tangentSize(const Block &block)
		{
			return block.manifold != nullptr ? block.manifold->TangentSize()
			                                 : static_cast<int>(block.linearization_point.size());
		}

		MatrixXd j;
		VectorXd e0;
		std::vector<Block> blocks;
	};

	/*
	 *
	 * Small helpers.
	 *
	 */

	Eigen::Quaterniond
	quat_of(const xrt_pose &p)
	{
		return Eigen::Quaterniond(p.orientation.w, p.orientation.x, p.orientation.y, p.orientation.z)
		    .normalized();
	}

	Eigen::Vector3d
	pos_of(const xrt_pose &p)
	{
		return Eigen::Vector3d(p.position.x, p.position.y, p.position.z);
	}

	xrt_pose
	make_pose(const Eigen::Quaterniond &q, const Eigen::Vector3d &t)
	{
		xrt_pose p;
		p.orientation = xrt_quat{(float)q.x(), (float)q.y(), (float)q.z(), (float)q.w()};
		p.position = xrt_vec3{(float)t.x(), (float)t.y(), (float)t.z()};
		return p;
	}

	Eigen::Isometry3d
	isometry_of(const xrt_pose &p)
	{
		Eigen::Isometry3d iso = Eigen::Isometry3d::Identity();
		iso.linear() = quat_of(p).toRotationMatrix();
		iso.translation() = pos_of(p);
		return iso;
	}

	//! IMU body state (world <- IMU pose, velocity) propagated with upstream's integrateOnce.
	struct BodyState
	{
		int64_t t;
		Eigen::Vector3d p;
		Eigen::Quaterniond q;
		Eigen::Vector3d v;
	};

	void
	integrate_once(BodyState &s,
	               int64_t sample_t,
	               const Eigen::Vector3d &accel,
	               const Eigen::Vector3d &gyro,
	               const ImuBias<double> &bias,
	               double gravity_mag)
	{
		const double dt = std::max(0.0, (double)(sample_t - s.t) * 1e-9);
		const Eigen::Vector3d a = (accel - bias.accel_bias).cwiseProduct(bias.accel_scale);
		const Eigen::Vector3d w = gyro - bias.gyro_bias;
		const Eigen::Vector3d world_accel = s.q * a + Eigen::Vector3d(0, gravity_mag, 0);
		s.p += s.v * dt + 0.5 * world_accel * dt * dt;
		s.v += world_accel * dt;
		s.q = (s.q * quat_exp_so3(Eigen::Vector3d(w * dt))).normalized();
		s.t = sample_t;
	}

} // namespace

const char *
fusion_update_status_name(FusionUpdateStatus status)
{
	switch (status) {
	case FusionUpdateStatus::Initialised: return "initialised";
	case FusionUpdateStatus::Accepted: return "accepted";
	case FusionUpdateStatus::RejectedGate: return "rejected_gate";
	case FusionUpdateStatus::RejectedPostSolve: return "rejected_post_solve";
	case FusionUpdateStatus::ResetAndInitialised: return "reset_initialised";
	case FusionUpdateStatus::Ignored: return "ignored";
	}
	return "?";
}

/*
 *
 * Implementation.
 *
 */

struct SlidingWindowFusion::Impl
{
	struct Keyframe
	{
		uint64_t id;
		int64_t t;
		std::vector<FusionCameraObservation> observations;
		Eigen::Quaterniond q_seed_model;
		SeedableVector<double, kPoseSize> pose;
		SeedableVector<double, kBiasSize> bias;
		//! The IMU interval ending at this keyframe, absent for the first keyframe of the window.
		bool has_preintegration{false};
		PreintegratedImuSamples preintegration;
	};

	struct PriorBlock
	{
		enum class Kind
		{
			Extrinsics,
			Pose,
			Bias,
		};
		Kind kind;
		uint64_t keyframe_id;
		std::vector<double> linearization_point;
	};

	struct Prior
	{
		bool valid{false};
		std::vector<PriorBlock> blocks;
		MatrixXd j;
		VectorXd e0;
	};

	//! What a rejected keyframe restores.
	struct Snapshot
	{
		std::deque<Keyframe> window;
		Prior prior;
		SeedableVector<double, kExtrinsicsSize> extrinsics;
		uint64_t next_keyframe_id;
	};

	struct Published
	{
		BodyState state;
		ImuBias<double> bias;
		Eigen::Quaterniond Q_imu_model;
	};

	SlidingWindowFusionParams params;
	FusionStats stats;

	std::deque<xrt_imu_sample> imu;
	std::deque<Keyframe> window;
	uint64_t next_keyframe_id{0};
	Prior prior;

	SeedableVector<double, kGravitySize> gravity;
	SeedableVector<double, kExtrinsicsSize> extrinsics;
	ImuBias<double> latest_bias;

	std::deque<Published> published;
	int64_t last_accepted_ns{INT64_MIN};
	uint32_t consecutive_rejections{0};
	bool have_epoch{false};
	uint32_t epoch{0};

	QuaternionManifold quaternion_manifold;
	PoseWithVelocityManifold pose_manifold;

	// Per build.
	std::unique_ptr<ceres::Problem> problem;
	std::vector<std::vector<ceres::ResidualBlockId>> keyframe_residuals;
	std::vector<std::vector<ceres::ResidualBlockId>> keyframe_camera_residuals;
	std::map<const double *, std::pair<PriorBlock::Kind, uint64_t>> block_refs;

	explicit Impl(const SlidingWindowFusionParams &p) : params(p)
	{
		// Eviction needs a keyframe left to hold the prior.
		params.window_size = std::max<uint32_t>(2, params.window_size);
		WorldGravity<double>().pack(gravity.seedVec());
		ImuExtrinsics<double>(params.Q_imu_model.normalized()).pack(extrinsics.seedVec());
		latest_bias = calibratedBias();
	}

	ImuBias<double>
	calibratedBias() const
	{
		return ImuBias<double>(params.calibrated_accel_bias, params.calibrated_gyro_bias,
		                       Eigen::Vector3d::Ones());
	}

	/*
	 * IMU buffer.
	 */

	void
	trimImu()
	{
		// Keep what the newest keyframe (preintegration start) and the published states still need.
		int64_t keep_from = INT64_MAX;
		if (!window.empty()) {
			keep_from = std::min(keep_from, window.back().t);
		}
		if (!published.empty()) {
			keep_from = std::min(keep_from, published.back().state.t);
		}
		keep_from = keep_from == INT64_MAX ? INT64_MIN : keep_from - 100'000'000;
		while (!imu.empty() && (imu.front().timestamp_ns < keep_from || imu.size() > params.max_imu_samples)) {
			imu.pop_front();
			stats.imu_samples_trimmed++;
		}
	}

	//! Samples in (start, end] and the first after end (or the last sample, held, if none has arrived).
	bool
	collectImu(int64_t start, int64_t end, std::vector<xrt_imu_sample> &out, xrt_imu_sample &first_after) const
	{
		out.clear();
		bool have_after = false;
		for (const xrt_imu_sample &s : imu) {
			if (s.timestamp_ns <= start) {
				continue;
			}
			if (s.timestamp_ns > end) {
				first_after = s;
				have_after = true;
				break;
			}
			out.push_back(s);
		}
		if (!have_after) {
			if (out.empty()) {
				return false;
			}
			first_after = out.back();
		}
		return true;
	}

	/*
	 * Prediction.
	 */

	BodyState
	propagate(const Published &from, int64_t t) const
	{
		BodyState s = from.state;
		const double g = WorldGravity<double>(gravity.vec).gravity_mag;
		const xrt_imu_sample *after = nullptr;
		const xrt_imu_sample *last = nullptr;
		for (const xrt_imu_sample &sample : imu) {
			if (sample.timestamp_ns <= s.t) {
				last = &sample;
				continue;
			}
			if (sample.timestamp_ns > t) {
				after = &sample;
				break;
			}
			integrate_once(s, sample.timestamp_ns, map_vec3_f64(sample.accel_m_s2),
			               map_vec3_f64(sample.gyro_rad_secs), from.bias, g);
			last = &sample;
		}
		// The tail up to t: the reading that ends the interval, or the last one held.
		const xrt_imu_sample *tail = after != nullptr ? after : last;
		if (tail != nullptr && t > s.t) {
			integrate_once(s, t, map_vec3_f64(tail->accel_m_s2), map_vec3_f64(tail->gyro_rad_secs),
			               from.bias, g);
		}
		s.t = std::max(s.t, t);
		return s;
	}

	/*
	 * Problem building (adapted from upstream SensorFusionProblem::buildLocked).
	 */

	double *
	resolve(const PriorBlock &block)
	{
		if (block.kind == PriorBlock::Kind::Extrinsics) {
			return extrinsics.vec.data();
		}
		for (Keyframe &kf : window) {
			if (kf.id == block.keyframe_id) {
				return block.kind == PriorBlock::Kind::Pose ? kf.pose.vec.data() : kf.bias.vec.data();
			}
		}
		return nullptr;
	}

	const ceres::Manifold *
	manifoldFor(PriorBlock::Kind kind) const
	{
		switch (kind) {
		case PriorBlock::Kind::Extrinsics: return &quaternion_manifold;
		case PriorBlock::Kind::Pose: return &pose_manifold;
		case PriorBlock::Kind::Bias: return nullptr;
		}
		return nullptr;
	}

	bool
	priorNames(PriorBlock::Kind kind, uint64_t id) const
	{
		if (!prior.valid) {
			return false;
		}
		for (const PriorBlock &b : prior.blocks) {
			if (b.kind == kind && (kind == PriorBlock::Kind::Extrinsics || b.keyframe_id == id)) {
				return true;
			}
		}
		return false;
	}

	void
	build()
	{
		ceres::Problem::Options options{};
		options.manifold_ownership = ceres::DO_NOT_TAKE_OWNERSHIP;
		options.loss_function_ownership = ceres::TAKE_OWNERSHIP;
		options.enable_fast_removal = true;
		problem = std::make_unique<ceres::Problem>(options);
		keyframe_residuals.assign(window.size(), {});
		keyframe_camera_residuals.assign(window.size(), {});
		block_refs.clear();

		problem->AddParameterBlock(gravity.data(), kGravitySize);
		problem->SetParameterBlockConstant(gravity.data());

		problem->AddParameterBlock(extrinsics.data(), kExtrinsicsSize, &quaternion_manifold);
		block_refs[extrinsics.vec.data()] = {PriorBlock::Kind::Extrinsics, 0};
		if (!params.optimize_extrinsics) {
			problem->SetParameterBlockConstant(extrinsics.data());
		}

		const ImuNoiseModel &noise = params.noise;
		for (Keyframe &kf : window) {
			problem->AddParameterBlock(kf.pose.data(), kPoseSize, &pose_manifold);
			problem->AddParameterBlock(kf.bias.data(), kBiasSize);
			block_refs[kf.pose.vec.data()] = {PriorBlock::Kind::Pose, kf.id};
			block_refs[kf.bias.vec.data()] = {PriorBlock::Kind::Bias, kf.id};
			for (int i = 0; i < 3; i++) {
				double *b = kf.bias.data();
				problem->SetParameterLowerBound(b, ImuBias<double>::kAccelScaleIndex + i,
				                                noise.min_accel_scale);
				problem->SetParameterUpperBound(b, ImuBias<double>::kAccelScaleIndex + i,
				                                noise.max_accel_scale);
				problem->SetParameterLowerBound(b, ImuBias<double>::kAccelBiasIndex + i,
				                                -noise.max_accel_bias);
				problem->SetParameterUpperBound(b, ImuBias<double>::kAccelBiasIndex + i,
				                                noise.max_accel_bias);
				problem->SetParameterLowerBound(b, ImuBias<double>::kGyroBiasIndex + i,
				                                -noise.max_gyro_bias);
				problem->SetParameterUpperBound(b, ImuBias<double>::kGyroBiasIndex + i,
				                                noise.max_gyro_bias);
			}
		}

		// The prior, if everything it names is still in the window.
		if (prior.valid) {
			std::vector<double *> parameters;
			std::vector<MarginalizationPriorCostFunction::Block> blocks;
			size_t oldest = window.size();
			for (const PriorBlock &b : prior.blocks) {
				double *values = resolve(b);
				if (values == nullptr) {
					prior.valid = false;
					stats.priors_dropped++;
					break;
				}
				parameters.push_back(values);
				blocks.push_back({b.linearization_point, manifoldFor(b.kind)});
				for (size_t i = 0; i < window.size(); i++) {
					if (b.kind != PriorBlock::Kind::Extrinsics && window[i].id == b.keyframe_id) {
						oldest = std::min(oldest, i);
					}
				}
			}
			if (prior.valid) {
				ceres::ResidualBlockId id = problem->AddResidualBlock(
				    new MarginalizationPriorCostFunction(prior.j, prior.e0, std::move(blocks)), nullptr,
				    parameters);
				keyframe_residuals[oldest < window.size() ? oldest : 0].push_back(id);
			}
		}

		// Only the poses the prior covers stay anchored (upstream releaseAnchorsOutsidePriorLocked).
		for (Keyframe &kf : window) {
			if (!priorNames(PriorBlock::Kind::Pose, kf.id)) {
				kf.pose.releaseAnchor();
			}
		}

		// Camera factors.
		for (size_t k = 0; k < window.size(); k++) {
			Keyframe &kf = window[k];
			for (const FusionCameraObservation &obs : kf.observations) {
				const int num_residuals = static_cast<int>(obs.points2d.size()) * kNumLedResiduals;
				if (num_residuals == 0) {
					continue;
				}
				xrt_pose Tcv_cam_world;
				math_pose_invert(&obs.Tcv_world_cam, &Tcv_cam_world);
				auto *functor =
				    new CameraObservationCostFunctor{obs.model, obs.points2d, obs.points3d,
				                                     isometry_of(Tcv_cam_world), params.blob_sigma_px};
				const double dof = static_cast<double>(num_residuals);
				const double threshold =
				    std::sqrt(dof + params.huber_delta_sigmas * std::sqrt(2.0 * dof));
				ceres::ResidualBlockId id = problem->AddResidualBlock(
				    new FirstEstimateJacobianCostFunction(
				        new CameraObservationCostFunction(functor, num_residuals),
				        {{kf.pose.anchorData(), &pose_manifold}, {nullptr, &quaternion_manifold}}),
				    new ceres::HuberLoss(threshold), kf.pose.data(), extrinsics.data());
				keyframe_residuals[k].push_back(id);
				keyframe_camera_residuals[k].push_back(id);
			}
		}

		if (params.seed_orientation_sigma_deg > 0.0) {
			for (size_t k = 0; k < window.size(); k++) {
				Keyframe &kf = window[k];
				ceres::ResidualBlockId id = problem->AddResidualBlock(
				    new FirstEstimateJacobianCostFunction(
				        new SeedOrientationCostFunction(new SeedOrientationCostFunctor{
				            kf.q_seed_model, params.seed_orientation_sigma_deg * M_PI / 180.0}),
				        {{kf.pose.anchorData(), &pose_manifold}, {nullptr, &quaternion_manifold}}),
				    nullptr, kf.pose.data(), extrinsics.data());
				keyframe_residuals[k].push_back(id);
			}
		}

		// IMU and bias random-walk factors, owned by the older keyframe of each pair.
		for (size_t k = 1; k < window.size(); k++) {
			Keyframe &start = window[k - 1];
			Keyframe &end = window[k];
			if (!end.has_preintegration) {
				continue;
			}
			ceres::ResidualBlockId imu_id = problem->AddResidualBlock(
			    new FirstEstimateJacobianCostFunction(
			        new ImuCostFunction(new ImuCostFunctor{end.preintegration}),
			        {{nullptr, nullptr},
			         {nullptr, nullptr},
			         {start.pose.anchorData(), &pose_manifold},
			         {end.pose.anchorData(), &pose_manifold}}),
			    nullptr, gravity.data(), start.bias.data(), start.pose.data(), end.pose.data());
			keyframe_residuals[k - 1].push_back(imu_id);

			ceres::ResidualBlockId walk_id = problem->AddResidualBlock(
			    new ImuBiasRandomWalkCostFunction(
			        new ImuBiasRandomWalkCostFunctor{std::max(1e-4, end.preintegration.dt), params.noise}),
			    nullptr, start.bias.data(), end.bias.data());
			keyframe_residuals[k - 1].push_back(walk_id);
		}

		// Anchor the oldest bias when no prior carries it.
		if (!window.empty() && !priorNames(PriorBlock::Kind::Bias, window.front().id)) {
			ceres::ResidualBlockId id = problem->AddResidualBlock(
			    new ImuBiasAnchorCostFunction(new ImuBiasAnchorCostFunctor{calibratedBias(), params.noise}),
			    nullptr, window.front().bias.data());
			keyframe_residuals[0].push_back(id);
		}
	}

	//! Unrobustified RMS reprojection error per LED (pixels, as M1 reports it) of a camera factor.
	double
	cameraRmsPx(ceres::ResidualBlockId id) const
	{
		double cost = 0.0;
		const int n = problem->GetCostFunctionForResidualBlock(id)->num_residuals();
		std::vector<double> residuals(n);
		problem->EvaluateResidualBlock(id, false, &cost, residuals.data(), nullptr);
		// cost = 0.5 sum (r / sigma)^2 over n / 2 LEDs.
		return params.blob_sigma_px * std::sqrt(4.0 * cost / std::max(1, n));
	}

	double
	keyframeRmsPx(size_t k) const
	{
		double sum2 = 0.0;
		int n = 0;
		for (ceres::ResidualBlockId id : keyframe_camera_residuals[k]) {
			const int m = problem->GetCostFunctionForResidualBlock(id)->num_residuals();
			const double rms = cameraRmsPx(id);
			sum2 += rms * rms * m;
			n += m;
		}
		return n > 0 ? std::sqrt(sum2 / n) : 0.0;
	}

	//! RMS reprojection (pixels) of an exposure's observations at an LED model pose.
	double
	reprojectionRmsPx(const FusionExposure &exposure, const xrt_pose &Tcv_world_model) const
	{
		const Eigen::Isometry3d world_model = isometry_of(Tcv_world_model);
		double sum2 = 0.0;
		int n = 0;
		for (const FusionCameraObservation &obs : exposure.observations) {
			const Eigen::Isometry3d cam_model = isometry_of(obs.Tcv_world_cam).inverse() * world_model;
			const Eigen::Quaterniond q(cam_model.linear());
			const Eigen::Vector3d t = cam_model.translation();
			for (size_t i = 0; i < obs.points2d.size(); i++) {
				double r[2];
				computeLedResidual<double>(obs.model, t, q, obs.points2d[i], obs.points3d[i], r);
				sum2 += r[0] * r[0] + r[1] * r[1];
				n += 2;
			}
		}
		return n > 0 ? std::sqrt(2.0 * sum2 / n) : 0.0;
	}

	/*
	 * Eviction (adapted from upstream SensorFusionProblem::evictLocked): fold the oldest keyframe's factors into a
	 * new prior over what they connect to, then drop it.
	 */
	void
	evictOldest()
	{
		build();

		Keyframe &oldest = window.front();
		std::set<double *> base = {oldest.pose.data(), oldest.bias.data()};

		// Leave out camera factors that are still outliers: they are what a bad solve would poison the prior
		// with.
		std::vector<ceres::ResidualBlockId> residuals;
		for (ceres::ResidualBlockId id : keyframe_residuals[0]) {
			const bool is_camera =
			    std::find(keyframe_camera_residuals[0].begin(), keyframe_camera_residuals[0].end(), id) !=
			    keyframe_camera_residuals[0].end();
			if (is_camera && cameraRmsPx(id) > params.marginalise_max_rms_px) {
				stats.factors_excluded_from_prior++;
				continue;
			}
			residuals.push_back(id);
		}

		std::vector<double *> connected;
		for (ceres::ResidualBlockId id : residuals) {
			std::vector<double *> blocks;
			problem->GetParameterBlocksForResidualBlock(id, &blocks);
			for (double *b : blocks) {
				if (base.count(b) != 0 || problem->IsParameterBlockConstant(b) ||
				    std::find(connected.begin(), connected.end(), b) != connected.end()) {
					continue;
				}
				connected.push_back(b);
			}
		}

		prior = Prior{};
		if (!connected.empty() && !residuals.empty()) {
			ceres::Problem::EvaluateOptions options;
			options.apply_loss_function = true;
			options.num_threads = 1;
			options.parameter_blocks = {oldest.pose.data(), oldest.bias.data()};
			options.parameter_blocks.insert(options.parameter_blocks.end(), connected.begin(),
			                                connected.end());
			options.residual_blocks = residuals;

			std::vector<double> residual_vec;
			ceres::CRSMatrix jacobian_crs;
			problem->Evaluate(options, nullptr, &residual_vec, nullptr, &jacobian_crs);

			MatrixXd jacobian;
			matrixCrsToEigen(jacobian_crs, jacobian);
			const VectorXd r = Map<VectorXd>(residual_vec.data(), residual_vec.size());
			const int num_base = problem->ParameterBlockTangentSize(oldest.pose.data()) +
			                     problem->ParameterBlockTangentSize(oldest.bias.data());

			VectorXd e0;
			MatrixXd j;
			marginalize(r, jacobian, num_base, e0, j);

			for (double *b : connected) {
				auto ref = block_refs.at(b);
				PriorBlock block{ref.first, ref.second, {}};
				const double *point = b;
				int size = kBiasSize;
				if (ref.first == PriorBlock::Kind::Pose) {
					for (Keyframe &kf : window) {
						if (kf.id == ref.second) {
							kf.pose.anchor();
							point = kf.pose.anchorData();
						}
					}
					size = kPoseSize;
				} else if (ref.first == PriorBlock::Kind::Extrinsics) {
					size = kExtrinsicsSize;
				}
				block.linearization_point.assign(point, point + size);
				prior.blocks.push_back(std::move(block));
			}
			prior.j = std::move(j);
			prior.e0 = std::move(e0);
			prior.valid = true;
			stats.marginalisations++;
		}

		window.pop_front();
	}

	void
	solve(int &iterations)
	{
		build();
		ceres::Solver::Options options{};
		options.max_num_iterations = params.max_iterations;
		options.linear_solver_type = ceres::DENSE_QR;
		options.num_threads = 1;
		options.logging_type = ceres::SILENT;
		ceres::Solver::Summary summary;
		ceres::Solve(options, problem.get(), &summary);
		iterations = static_cast<int>(summary.iterations.size());
	}

	/*
	 * State handling.
	 */

	void
	resetState()
	{
		latest_bias = calibratedBias();
		window.clear();
		prior = Prior{};
		published.clear();
		last_accepted_ns = INT64_MIN;
		consecutive_rejections = 0;
	}

	void
	publishNewest()
	{
		const Keyframe &kf = window.back();
		const PoseWithVelocity<double> pv{kf.pose.vec};
		Published p;
		p.state = BodyState{kf.t, pv.translation, pv.rotation.normalized(), pv.velocity};
		p.bias = ImuBias<double>(kf.bias.vec);
		p.Q_imu_model = ImuExtrinsics<double>(extrinsics.vec).Q_imu_model.normalized();
		published.push_back(p);
		while (published.size() > params.max_published_states) {
			published.pop_front();
		}
		latest_bias = p.bias;
		stats.gyro_bias = p.bias.gyro_bias;
		stats.accel_bias = p.bias.accel_bias;
		stats.Q_imu_model = p.Q_imu_model;
	}

	Keyframe
	makeKeyframe(const FusionExposure &exposure, const Eigen::Vector3d &velocity)
	{
		Keyframe kf;
		kf.id = next_keyframe_id++;
		kf.t = exposure.timestamp_ns;
		kf.observations = exposure.observations;
		kf.q_seed_model = quat_of(exposure.Tcv_world_model_seed);

		// The seed is the LED model's pose; the state is the IMU body's: Q_world_imu = Q_world_model
		// Q_imu_model^-1.
		const Eigen::Quaterniond q_imu_model = ImuExtrinsics<double>(extrinsics.vec).Q_imu_model.normalized();
		const Eigen::Quaterniond q_world_imu = quat_of(exposure.Tcv_world_model_seed) * q_imu_model.conjugate();
		PoseWithVelocity<double>(make_pose(q_world_imu, pos_of(exposure.Tcv_world_model_seed)), velocity)
		    .pack(kf.pose.seedVec());
		latest_bias.pack(kf.bias.seedVec());
		return kf;
	}

	FusionUpdateResult
	initialise(const FusionExposure &exposure, FusionUpdateStatus status)
	{
		resetState();
		window.push_back(makeKeyframe(exposure, Eigen::Vector3d::Zero()));
		publishNewest();
		last_accepted_ns = exposure.timestamp_ns;
		stats.initialisations++;
		FusionUpdateResult result;
		result.status = status;
		return result;
	}

	FusionUpdateResult
	push(const FusionExposure &exposure)
	{
		stats.exposures++;
		FusionUpdateResult result;
		if (exposure.observations.empty()) {
			return result;
		}

		if (!have_epoch || exposure.sync_epoch != epoch) {
			if (have_epoch && !window.empty()) {
				result.epoch_reset = true;
				stats.resets_epoch++;
				resetState();
			}
			have_epoch = true;
			epoch = exposure.sync_epoch;
		}

		if (window.empty()) {
			FusionUpdateResult r = initialise(exposure, FusionUpdateStatus::Initialised);
			r.epoch_reset = result.epoch_reset;
			return r;
		}
		if (exposure.timestamp_ns <= window.back().t) {
			return result;
		}
		if (exposure.timestamp_ns - last_accepted_ns > params.reset_gap_ns) {
			stats.resets_gap++;
			FusionUpdateResult r = initialise(exposure, FusionUpdateStatus::ResetAndInitialised);
			r.reason = "gap";
			return r;
		}

		// Prediction from the newest keyframe, for the gate and the velocity seed.
		const Published &newest = published.back();
		const BodyState predicted = propagate(newest, exposure.timestamp_ns);
		const Eigen::Quaterniond predicted_model = predicted.q * newest.Q_imu_model;
		const double dt = (double)(exposure.timestamp_ns - newest.state.t) * 1e-9;
		result.gate_position_error_m = (pos_of(exposure.Tcv_world_model_seed) - predicted.p).norm();
		result.gate_orientation_error_deg =
		    predicted_model.angularDistance(quat_of(exposure.Tcv_world_model_seed)) * 180.0 / M_PI;

		auto reject = [&](FusionUpdateStatus status, const char *why) {
			result.status = status;
			result.reason = why;
			if (++consecutive_rejections >= params.reset_after_rejections) {
				stats.resets_rejections++;
				FusionUpdateResult r = initialise(exposure, FusionUpdateStatus::ResetAndInitialised);
				r.reason = std::string("rejections:") + why;
				r.gate_position_error_m = result.gate_position_error_m;
				r.gate_orientation_error_deg = result.gate_orientation_error_deg;
				return r;
			}
			if (status == FusionUpdateStatus::RejectedGate) {
				stats.rejected_gate++;
			} else {
				stats.rejected_post_solve++;
			}
			return result;
		};

		result.gate_reprojection_px =
		    reprojectionRmsPx(exposure, make_pose(predicted_model, pos_of(exposure.Tcv_world_model_seed)));
		if (params.gate) {
			const double pos_gate = params.gate_position_m + 0.5 * params.gate_accel_m_s2 * dt * dt;
			const double rot_gate = params.gate_orientation_deg + params.gate_rate_deg_s * dt;
			if (result.gate_position_error_m > pos_gate) {
				return reject(FusionUpdateStatus::RejectedGate, "position");
			}
			if (result.gate_reprojection_px > params.gate_reprojection_px) {
				return reject(FusionUpdateStatus::RejectedGate, "reprojection");
			}
			if (result.gate_orientation_error_deg > rot_gate) {
				return reject(FusionUpdateStatus::RejectedGate, "orientation");
			}
		}

		// Preintegrate from the newest keyframe to this exposure, with the latest bias.
		std::vector<xrt_imu_sample> samples;
		xrt_imu_sample first_after{};
		if (!collectImu(window.back().t, exposure.timestamp_ns, samples, first_after)) {
			FusionUpdateResult r = initialise(exposure, FusionUpdateStatus::ResetAndInitialised);
			r.reason = "no_imu";
			return r;
		}

		Snapshot snapshot{window, prior, extrinsics, next_keyframe_id};
		const int64_t previous_t = window.back().t;
		const bool second_keyframe = window.size() == 1 && published.size() == 1;

		if (window.size() >= params.window_size) {
			evictOldest();
		}

		// Velocity seed: the IMU prediction, or (for a second keyframe with no velocity yet) a seed difference.
		Eigen::Vector3d velocity = predicted.v;
		if (second_keyframe) {
			velocity = (pos_of(exposure.Tcv_world_model_seed) - newest.state.p) / std::max(dt, 1e-3);
		}
		if (velocity.norm() > kMaxSeedVelocityMetersPerSecond) {
			velocity.setZero();
		}

		Keyframe kf = makeKeyframe(exposure, velocity);
		kf.preintegration = preintegrate(samples, first_after, latest_bias.seed(), previous_t,
		                                 exposure.timestamp_ns, params.noise);
		kf.has_preintegration = true;
		window.push_back(std::move(kf));

		auto start = std::chrono::steady_clock::now();
		solve(result.iterations);
		result.solve_us =
		    std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
		result.solved_rms_px = keyframeRmsPx(window.size() - 1);

		if (result.solved_rms_px > params.post_solve_max_rms_px) {
			window = std::move(snapshot.window);
			prior = std::move(snapshot.prior);
			extrinsics = snapshot.extrinsics;
			next_keyframe_id = snapshot.next_keyframe_id;
			return reject(FusionUpdateStatus::RejectedPostSolve, "residual");
		}

		consecutive_rejections = 0;
		last_accepted_ns = exposure.timestamp_ns;
		publishNewest();
		stats.accepted++;
		result.status = FusionUpdateStatus::Accepted;
		return result;
	}

	FusionPose
	pose(int64_t t) const
	{
		FusionPose out;
		const Published *from = nullptr;
		for (auto it = published.rbegin(); it != published.rend(); ++it) {
			if (it->state.t <= t) {
				from = &*it;
				break;
			}
		}
		if (from == nullptr) {
			return out;
		}
		const BodyState s = propagate(*from, t);
		const Eigen::Quaterniond q_model = (s.q * from->Q_imu_model).normalized();
		out.Tcv_world_model = make_pose(q_model, s.p);
		out.velocity = s.v;
		out.orientation_valid = true;
		out.position_valid = t - last_accepted_ns <= params.position_valid_ns;

		// Angular velocity from the reading nearest t.
		const xrt_imu_sample *nearest = nullptr;
		for (const xrt_imu_sample &sample : imu) {
			if (nearest == nullptr ||
			    std::llabs(sample.timestamp_ns - t) < std::llabs(nearest->timestamp_ns - t)) {
				nearest = &sample;
			}
		}
		if (nearest != nullptr) {
			out.angular_velocity = s.q * (map_vec3_f64(nearest->gyro_rad_secs) - from->bias.gyro_bias);
		}
		return out;
	}
};

SlidingWindowFusion::SlidingWindowFusion(const SlidingWindowFusionParams &params) : impl(std::make_unique<Impl>(params))
{}

SlidingWindowFusion::~SlidingWindowFusion() = default;

void
SlidingWindowFusion::pushImu(const xrt_imu_sample &sample)
{
	if (!impl->imu.empty() && sample.timestamp_ns <= impl->imu.back().timestamp_ns) {
		return;
	}
	impl->imu.push_back(sample);
	impl->stats.imu_samples++;
	impl->trimImu();
	impl->stats.peak_imu_buffer = std::max(impl->stats.peak_imu_buffer, impl->imu.size());
}

FusionUpdateResult
SlidingWindowFusion::pushExposure(const FusionExposure &exposure)
{
	FusionUpdateResult result = impl->push(exposure);
	impl->trimImu();
	return result;
}

FusionPose
SlidingWindowFusion::getPose(int64_t timestamp_ns) const
{
	return impl->pose(timestamp_ns);
}

const FusionStats &
SlidingWindowFusion::stats() const
{
	return impl->stats;
}

void
SlidingWindowFusion::reset()
{
	impl->resetState();
	impl->have_epoch = false;
}

} // namespace xrt::tracking::constellation::fusion
