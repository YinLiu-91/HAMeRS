#ifndef EULER_HPP
#define EULER_HPP

#include "SAMRAI/SAMRAI_config.h"

#include "SAMRAI/appu/VisDerivedDataStrategy.h"
#include "SAMRAI/appu/VisItDataWriter.h"
#include "SAMRAI/geom/CartesianGridGeometry.h"
#include "SAMRAI/hier/BoundaryBox.h"
#include "SAMRAI/hier/Box.h"
#include "SAMRAI/hier/IntVector.h"
#include "SAMRAI/hier/Patch.h"
#include "SAMRAI/hier/VariableContext.h"
#include "SAMRAI/mesh/GriddingAlgorithm.h"
#include "SAMRAI/pdat/CellVariable.h"
#include "SAMRAI/pdat/FaceData.h"
#include "SAMRAI/pdat/FaceVariable.h"
#include "SAMRAI/tbox/Database.h"
#include "SAMRAI/tbox/MessageStream.h"
#include "SAMRAI/tbox/Serializable.h"

#include "flow_model/FlowModels.hpp"
#include "flow_model/FlowModelManager.hpp"
#include "flow_model/boundary_conditions/Euler/EulerBoundaryConditions.hpp"
#include "flow_model/convective_flux_reconstructor/ConvectiveFluxReconstructor.hpp"
#include "flow_model/feature_driven_tagger/FeatureDrivenTagger.hpp"
#include "flow_model/initial_conditions/InitialConditions.hpp"
#include "integrator/RungeKuttaLevelIntegrator.hpp"
#include "patch_strategy/RungeKuttaPatchStrategy.hpp"

#include "boost/shared_ptr.hpp"
#include <string>
#include <vector>

using namespace SAMRAI;

/**
 * The Euler class provides routines for a sample application code that
 * solves the Euler equations of gas dynamics.  This code illustrates the
 * manner in which a code employing the standard Berger/Oliger AMR algorithm
 * for explicit hydrodynamics can be used in the SAMRAI framework.
 * This class is derived from the RungeKuttaPatchStrategy abstract base
 * class which defines the bulk of the interface between the hyperbolic
 * Runge-Kutta intergration algorithm modified from SAMRAI's implementation of
 * algs::HyperbolicPatchStrategyand the numerical routines specific to Euler.
 * In particular, this class provides routines which maybe applied to any patch
 * in an AMR patch hierarchy.
 *
 * The numerical routines model the Euler equations of gas dynamics with
 * explicit timestepping and a second-order unsplit Godunov method.
 * The primary numerical quantities are density, velocity, and pressure.
 */

class Euler:
    public RungeKuttaPatchStrategy,
    public tbox::Serializable
{
    public:
        /**
         * The constructor for Euler sets default parameters for the
         * Euler model.  Specifically, it allocates the variables that represent
         * the state of the solution.  The constructor also registers this
         * object for restart with the restart manager using the object name.
         *
         * After default values are set, this routine calls getFromRestart()
         * if execution from a restart file is specified.  Finally,
         * getFromInput() is called to read values from the given input
         * database (potentially overriding those found in the restart file).
         */
        Euler(
            const std::string& object_name,
            const tbox::Dimension& dim,
            const boost::shared_ptr<tbox::Database>& input_db,
            const boost::shared_ptr<geom::CartesianGridGeometry>& grid_geometry);
        
        /**
         * Destructor of Euler.
         */
        ~Euler();
        
        /**
         * Register Euler model variables with RungeKuttaLevelIntegrator
         * according to variable registration function provided by the integrator.
         * In other words, variables are registered according to their role
         * in the integration process (e.g., time-dependent, flux, etc.).
         * This routine also registers variables for plotting with the
         * Vis writer.
         */
        void
        registerModelVariables(
            RungeKuttaLevelIntegrator* integrator);
        
        /**
         * Set up parameters in the load balancer object (owned by the gridding
         * algorithm) if needed.  The Euler model allows non-uniform load balancing
         * to be used based on the input file parameter called
         * "use_nonuniform_workload".  The default case is to use uniform
         * load balancing (i.e., use_nonuniform_workload == false).  For
         * illustrative and testing purposes, when non-uniform load balancing is
         * turned on, a weight of one will be applied to every grid cell.  This
         * should produce an identical patch configuration to the uniform load
         * balance case.
         */
        void
        setupLoadBalancer(
            RungeKuttaLevelIntegrator* integrator,
            mesh::GriddingAlgorithm* gridding_algorithm);
        
        /**
         * Set the data on the patch interior to some initial values,
         * depending on the input parameters and numerical routines.
         * If the "initial_time" flag is false, indicating that the
         * routine is called after a regridding step, the routine does nothing.
         */
        void
        initializeDataOnPatch(
            hier::Patch& patch,
            const double data_time,
            const bool initial_time);
        
        /**
         * Compute the stable time increment for patch using a CFL
         * condition and return the computed dt.
         */
        double
        computeStableDtOnPatch(
            hier::Patch& patch,
            const bool initial_time,
            const double dt_time);
        
        /**
         * Compute time integral of hyperbolic fluxes to be used in finite
         * difference for patch Runge-Kutta integration.
         * 
         * If the equations are in non-conservative form, the TIME INTEGRALS of the
         * correspoinding sources are also computed after the equation is hyperolized.
         *
         * The finite difference used to update the integrated quantities
         * through the Runge-Kutta steps is implemented in the advanceSingleStep()
         * routine.
         */
        void
        computeHyperbolicFluxesAndSourcesOnPatch(
            hier::Patch& patch,
            const double time,
            const double dt,
            const int RK_step_number);
        
        /**
         * Advance a single Runge-Kutta step. Conservative differencing is
         * implemented here by using the hyperbolic fluxes and sources computed
         * in computeHyperbolicFluxesAndSourcesOnPatch()
         */
        void
        advanceSingleStep(
            hier::Patch& patch,
            const double time,
            const double dt,
            const std::vector<double>& alpha,
            const std::vector<double>& beta,
            const std::vector<double>& gamma,
            const std::vector<boost::shared_ptr<hier::VariableContext> >& intermediate_context);
        
        /**
         * Correct Euler solution variables at coarse-fine booundaries by
         * repeating conservative differencing with corrected fluxes.
         */
        void
        synchronizeHyperbolicFluxes(
            hier::Patch& patch,
            const double time,
            const double dt);
        
        /**
         * Tag cells for refinement using gradient detector.
         */
        void
        tagGradientDetectorCells(
            hier::Patch& patch,
            const double regrid_time,
            const bool initial_error,
            const int tag_indx,
            const bool uses_richardson_extrapolation_too);
        
        //@{
        //! @name Required implementations of RungeKuttaPatchStrategy pure virtuals.
        
        ///
        ///  The following routines:
        ///
        ///      setPhysicalBoundaryConditions(),
        ///      getRefineOpStencilWidth(),
        ///      preprocessRefine()
        ///      postprocessRefine()
        ///
        ///  are concrete implementations of functions declared in the
        ///  RefinePatchStrategy abstract base class.
        ///
        
        /**
         * Set the data in ghost cells corresponding to physical boundary
         * conditions. Specific boundary conditions are determined by
         * information specified in input file and numerical routines.
         */
        void
        setPhysicalBoundaryConditions(
            hier::Patch& patch,
            const double fill_time,
            const hier::IntVector& ghost_width_to_fill);
        
        /**
         * Return stencil width of conservative linear interpolation operations.
         */
        hier::IntVector
        getRefineOpStencilWidth(
            const tbox::Dimension& dim) const
        {
           return hier::IntVector::getOne(dim);
        }
        
        void
        preprocessRefine(
            hier::Patch& fine,
            const hier::Patch& coarse,
            const hier::Box& fine_box,
            const hier::IntVector& ratio)
        {
            NULL_USE(fine);
            NULL_USE(coarse);
            NULL_USE(fine_box);
            NULL_USE(ratio);
        }
        
        void
        postprocessRefine(
            hier::Patch& fine,
            const hier::Patch& coarse,
            const hier::Box& fine_box,
            const hier::IntVector& ratio)
        {
            NULL_USE(fine);
            NULL_USE(coarse);
            NULL_USE(fine_box);
            NULL_USE(ratio);
        }
        
        ///
        ///  The following routines:
        ///
        ///      getCoarsenOpStencilWidth(),
        ///      preprocessCoarsen()
        ///      postprocessCoarsen()
        ///
        ///  are concrete implementations of functions declared in the
        ///  CoarsenPatchStrategy abstract base class.  They are trivial
        ///  because this class doesn't do any pre/postprocessCoarsen.
        ///
        
        /**
         * Return stencil width of conservative averaging operations.
         */
        hier::IntVector
        getCoarsenOpStencilWidth(
            const tbox::Dimension& dim) const
        {
            return hier::IntVector::getZero(dim);
        }
        
        void
        preprocessCoarsen(
            hier::Patch& coarse,
            const hier::Patch& fine,
            const hier::Box& coarse_box,
            const hier::IntVector& ratio)
        {
            NULL_USE(coarse);
            NULL_USE(fine);
            NULL_USE(coarse_box);
            NULL_USE(ratio);
        }
        
        void
        postprocessCoarsen(
            hier::Patch& coarse,
            const hier::Patch& fine,
            const hier::Box& coarse_box,
            const hier::IntVector& ratio)
        {
            NULL_USE(coarse);
            NULL_USE(fine);
            NULL_USE(coarse_box);
            NULL_USE(ratio);
        }
        
        //@}
        
        /**
         * Write state of Euler object to the given database for restart.
         *
         * This routine is a concrete implementation of the function
         * declared in the tbox::Serializable abstract base class.
         */
        void
        putToRestart(
            const boost::shared_ptr<tbox::Database>& restart_db) const;
        
        /**
         * Register a VisIt data writer so this class will write
         * plot files that may be postprocessed with the VisIt
         * visualization tool.
         */
#ifdef HAVE_HDF5
        void registerVisItDataWriter(const boost::shared_ptr<appu::VisItDataWriter>& viz_writer);
#endif
        
        /**
         * This routine is a concrete implementation of the virtual function
         * in the base class appu::VisDerivedDataStrategy.  It computes derived
         * plot quantities registered with the VisIt data
         * writers from data  that is maintained on each patch in the
         * hierarchy.  In particular, it writes the plot quantity
         * identified by the string variable name to the specified
         * double buffer on the patch in the given region.  The depth_id
         * integer argument indicates which entry in the "depth" of the
         * vector is being written; for a scalar quantity, this may be
         * ignored.  For a vector quantity, it may be used to compute
         * the quantity at the particular depth (e.g. mom[depth_id] =
         * rho * vel[depth_id]).  The boolean return value specifies
         * whether or not derived data exists on the patch.  Generally,
         * this will be TRUE.  If the packDerivedDataIntoDoubleBuffer data does NOT exist on
         * the patch, return FALSE.
         */
        bool
        packDerivedDataIntoDoubleBuffer(
            double* buffer,
            const hier::Patch& patch,
            const hier::Box& region,
            const std::string& variable_name,
            int depth_id,
            double simulation_time) const;
        
        ///
        ///  The following routines are specific to the Euler class and
        ///  are not declared in any base class.
        ///
        
        /**
         * Print all data members for Euler class.
         */
        void printClassData(std::ostream& os) const;
        
        /**
         * Print all data statistics for Euler class.
         */
        void
        printDataStatistics(
            std::ostream& os,
            const boost::shared_ptr<hier::PatchHierarchy>& patch_hierarchy) const;
        
    private:
        /*
         * These private member functions read data from input and restart.
         * When beginning a run from a restart file, all data members are read
         * from the restart file.  If the boolean flag is true when reading
         * from input, some restart values may be overridden by those in the
         * input file.
         *
         * An assertion results if the database pointer is null.
         */
        void
        getFromInput(
            const boost::shared_ptr<tbox::Database>& input_db,
            bool is_from_restart);
        
        void getFromRestart();
        
        void
        preservePositivity(
            std::vector<double*>& Q,
            const boost::shared_ptr<pdat::FaceData<double> >& convective_flux_intermediate,
            const boost::shared_ptr<pdat::CellData<double> >& source_intermediate,
            const hier::IntVector interior_dims,
            const hier::IntVector ghostcell_dims,
            const double* const dx,
            const double& dt,
            const double& beta);
        
        /*
         * The object name is used for error/warning reporting.
         */
        const std::string d_object_name;
        
        /*
         * Name of the project.
         */
        std::string d_project_name;
        
        /*
         * Problem dimension.
         */
        const tbox::Dimension d_dim;
        
        /*
         * We cache pointers to the grid geometry and Vis data writers
         * to set up initial data, set physical boundary conditions,
         * and register plot variables. We also cache a pointer to the
         * plot context passed to the variable registration routine.
         */
        const boost::shared_ptr<geom::CartesianGridGeometry> d_grid_geometry;
        
#ifdef HAVE_HDF5
        boost::shared_ptr<appu::VisItDataWriter> d_visit_writer;
#endif
        
        /*
         * Data iterms used for nonuniform load balance, if used.
         */
        boost::shared_ptr<pdat::CellVariable<double> > d_workload_variable;
        int d_workload_data_id;
        bool d_use_nonuniform_workload;
        
        /*
         * Number of ghost cells for time-independent variables.
         */
        hier::IntVector d_num_ghosts;
        
        /*
         * A string variable to describe the flow model used.
         */
        std::string d_flow_model_str;
        
        /*
         * Number of species.
         */
        int d_num_species;
        
        /*
         * boost::shared_ptr to EquationOfState and its database.
         */
        boost::shared_ptr<EquationOfState> d_equation_of_state;
        boost::shared_ptr<tbox::Database> d_equation_of_state_db;
        
        /*
         * boost::shared_ptr to the convective flux reconstructor and
         * to the database of the corresponding shock capturing scheme.
         */
        boost::shared_ptr<ConvectiveFluxReconstructor> d_conv_flux_reconstructor;
        boost::shared_ptr<tbox::Database> d_shock_capturing_scheme_db;
        
        /*
         * boost::shared_ptr to InitialConditions.
         */
        boost::shared_ptr<InitialConditions> d_initial_conditions;
        
        /*
         * boost::shared_ptr to EulerBoundaryConditions and its database.
         */
        boost::shared_ptr<EulerBoundaryConditions> d_Euler_boundary_conditions;
        boost::shared_ptr<tbox::Database> d_Euler_boundary_conditions_db;
        bool d_Euler_boundary_conditions_db_is_from_restart;
        
        /*
         * boost::shared_ptr to FeatureDrivenTagger and its database.
         */
        boost::shared_ptr<FeatureDrivenTagger> d_feature_driven_tagger;
        boost::shared_ptr<tbox::Database> d_feature_driven_tagger_db;
        
        
        /*
         * boost::shared_ptr to FlowModelManager.
         */
        boost::shared_ptr<FlowModelManager> d_flow_model_manager;
        
        /*
         * boost::shared_ptr to convective flux variable vector.
         */
        boost::shared_ptr<pdat::FaceVariable<double> > d_convective_flux;
        
        /*
         * boost::shared_ptr to source term.
         */
        boost::shared_ptr<pdat::CellVariable<double> > d_source;
        
        /*
         * Boolean to enable positivity-preserving.
         */
        bool d_is_preserving_positivity;
        
        /*
         * Timers.
         */
        static boost::shared_ptr<tbox::Timer> t_init;
        static boost::shared_ptr<tbox::Timer> t_compute_dt;
        static boost::shared_ptr<tbox::Timer> t_compute_hyperbolicfluxes;
        static boost::shared_ptr<tbox::Timer> t_advance_steps;
        static boost::shared_ptr<tbox::Timer> t_synchronize_hyperbloicfluxes;
        static boost::shared_ptr<tbox::Timer> t_setphysbcs;
        static boost::shared_ptr<tbox::Timer> t_taggradient;
};

#endif /* EULER_HPP */
