#include "apps/Navier-Stokes/NavierStokes.hpp"

#include "SAMRAI/geom/CartesianPatchGeometry.h"
#include "SAMRAI/hier/BoxContainer.h"
#include "SAMRAI/hier/Index.h"
#include "SAMRAI/hier/PatchDataRestartManager.h"
#include "SAMRAI/hier/VariableDatabase.h"
#include "SAMRAI/math/HierarchyCellDataOpsReal.h"
#include "SAMRAI/mesh/TreeLoadBalancer.h"
#include "SAMRAI/pdat/CellData.h"
#include "SAMRAI/pdat/CellIndex.h"
#include "SAMRAI/pdat/CellIterator.h"
#include "SAMRAI/pdat/FaceData.h"
#include "SAMRAI/pdat/FaceIndex.h"
#include "SAMRAI/pdat/FaceVariable.h"
#include "SAMRAI/tbox/MathUtilities.h"
#include "SAMRAI/tbox/SAMRAI_MPI.h"
#include "SAMRAI/tbox/PIO.h"
#include "SAMRAI/tbox/RestartManager.h"
#include "SAMRAI/tbox/Timer.h"
#include "SAMRAI/tbox/TimerManager.h"
#include "SAMRAI/tbox/Utilities.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>

#ifndef LACKS_SSTREAM
#ifndef included_sstream
#define included_sstream
#include <sstream>
#endif
#else
#ifndef included_strstream
#define included_strstream
#include <strstream>
#endif
#endif

boost::shared_ptr<tbox::Timer> NavierStokes::t_init;
boost::shared_ptr<tbox::Timer> NavierStokes::t_compute_dt;
boost::shared_ptr<tbox::Timer> NavierStokes::t_compute_hyperbolicfluxes;
boost::shared_ptr<tbox::Timer> NavierStokes::t_advance_steps;
boost::shared_ptr<tbox::Timer> NavierStokes::t_synchronize_hyperbloicfluxes;
boost::shared_ptr<tbox::Timer> NavierStokes::t_setphysbcs;
boost::shared_ptr<tbox::Timer> NavierStokes::t_tagvalue;
boost::shared_ptr<tbox::Timer> NavierStokes::t_taggradient;
boost::shared_ptr<tbox::Timer> NavierStokes::t_tagmultiresolution;

NavierStokes::NavierStokes(
    const std::string& object_name,
    const tbox::Dimension& dim,
    const boost::shared_ptr<tbox::Database>& input_db,
    const boost::shared_ptr<geom::CartesianGridGeometry>& grid_geometry):
        RungeKuttaPatchStrategy(),
        d_object_name(object_name),
        d_dim(dim),
        d_grid_geometry(grid_geometry),
        d_use_nonuniform_workload(false),
        d_num_ghosts(hier::IntVector::getZero(d_dim)),
        d_Navier_Stokes_boundary_conditions_db_is_from_restart(false)
{
    TBOX_ASSERT(!object_name.empty());
    TBOX_ASSERT(input_db);
    TBOX_ASSERT(grid_geometry);
    
    tbox::RestartManager::getManager()->registerRestartItem(d_object_name, this);
    
    if (!t_init)
    {
        t_init = tbox::TimerManager::getManager()->
            getTimer("NavierStokes::initializeDataOnPatch()");
        t_compute_dt = tbox::TimerManager::getManager()->
            getTimer("NavierStokes::computeStableDtOnPatch()");
        t_compute_hyperbolicfluxes = tbox::TimerManager::getManager()->
            getTimer("NavierStokes::computeHyperbolicFluxesOnPatch()");
        t_advance_steps = tbox::TimerManager::getManager()->
            getTimer("NavierStokes::advanceSingleStep()");
        t_synchronize_hyperbloicfluxes = tbox::TimerManager::getManager()->
            getTimer("NavierStokes::synchronizeHyperbolicFluxes()");
        t_setphysbcs = tbox::TimerManager::getManager()->
            getTimer("NavierStokes::setPhysicalBoundaryConditions()");
        t_tagvalue = tbox::TimerManager::getManager()->
            getTimer("NavierStokes::tagValueDetectorCells()");
        t_taggradient = tbox::TimerManager::getManager()->
            getTimer("NavierStokes::tagGradientDetectorCells()");
        t_tagmultiresolution = tbox::TimerManager::getManager()->
            getTimer("NavierStokes::tagMultiresolutionDetectorCells()");
    }
    
    /*
     * Initialize object with data read from given input/restart databases.
     */
    
    bool is_from_restart = tbox::RestartManager::getManager()->isFromRestart();
    if (is_from_restart)
    {
        getFromRestart();
    }
    getFromInput(input_db, is_from_restart);
    
    /*
     * Initialize d_flow_model_manager and get the flow model object.
     */
    
    d_flow_model_manager.reset(new FlowModelManager(
        "d_flow_model_manager",
        d_dim,
        d_grid_geometry,
        d_num_species,
        d_flow_model_db,
        d_flow_model_str));
    
    d_flow_model = d_flow_model_manager->getFlowModel();
    
    /*
     * Initialize d_convective_flux_reconstructor_manager and get the convective flux reconstructor object.
     */
    
    d_convective_flux_reconstructor_manager.reset(new ConvectiveFluxReconstructorManager(
        "d_convective_flux_reconstructor_manager",
        d_dim,
        d_grid_geometry,
        d_flow_model->getNumberOfEquations(),
        d_num_species,
        d_flow_model,
        d_convective_flux_reconstructor_db,
        d_convective_flux_reconstructor_str));
    
    d_convective_flux_reconstructor = d_convective_flux_reconstructor_manager->getConvectiveFluxReconstructor();
    
    /*
     * Initialize d_diffusive_flux_reconstructor_manager and get the diffusive flux reconstructor object.
     */
    
    d_diffusive_flux_reconstructor_manager.reset(new DiffusiveFluxReconstructorManager(
        "d_diffusive_flux_reconstructor_manager",
        d_dim,
        d_grid_geometry,
        d_flow_model->getNumberOfEquations(),
        d_num_species,
        d_flow_model,
        d_diffusive_flux_reconstructor_db,
        d_diffusive_flux_reconstructor_str));
    
    d_diffusive_flux_reconstructor = d_diffusive_flux_reconstructor_manager->getDiffusiveFluxReconstructor();
    
    /*
     * Initialize d_Navier_Stokes_initial_conditions.
     */
    
    d_Navier_Stokes_initial_conditions.reset(new NavierStokesInitialConditions(
        "d_Navier_Stokes_initial_conditions",
        d_project_name,
        d_dim,
        d_grid_geometry,
        d_flow_model_manager->getFlowModelType(),
        d_num_species));
    
    d_Navier_Stokes_initial_conditions->setVariables(d_flow_model->getConservativeVariables());
    
    /*
     * Initialize d_Navier_Stokes_boundary_conditions.
     */
    
    d_Navier_Stokes_boundary_conditions.reset(new NavierStokesBoundaryConditions(
        "d_Navier_Stokes_boundary_conditions",
        d_project_name,
        d_dim,
        d_grid_geometry,
        d_num_species,
        d_flow_model,
        d_Navier_Stokes_boundary_conditions_db,
        d_Navier_Stokes_boundary_conditions_db_is_from_restart));
    
    /*
     * Initialize d_value_tagger.
     */
    
    if (d_value_tagger_db != nullptr)
    {
        d_value_tagger.reset(new ValueTagger(
            "d_value_tagger",
            d_dim,
            d_grid_geometry,
            d_num_species,
            d_flow_model,
            d_value_tagger_db));
    }
    
    /*
     * Initialize d_gradient_tagger.
     */
    
    if (d_gradient_tagger_db != nullptr)
    {
        d_gradient_tagger.reset(new GradientTagger(
            "d_gradient_tagger",
            d_dim,
            d_grid_geometry,
            d_num_species,
            d_flow_model,
            d_gradient_tagger_db));
    }
    
    /*
     * Initialize d_multiresolution_tagger.
     */
    
    if (d_multiresolution_tagger_db != nullptr)
    {
        d_multiresolution_tagger.reset(new MultiresolutionTagger(
            "multiresolution tagger",
            d_dim,
            d_grid_geometry,
            d_num_species,
            d_flow_model,
            d_multiresolution_tagger_db));
    }
    
    /*
     * Determine the number of ghost cells needed.
     */
    
    if (!is_from_restart)
    {
        d_num_ghosts = hier::IntVector::getZero(d_dim);
        
        d_num_ghosts = hier::IntVector::max(
            d_num_ghosts,
            d_convective_flux_reconstructor->getConvectiveFluxNumberOfGhostCells());
        
        d_num_ghosts = hier::IntVector::max(
            d_num_ghosts,
            d_diffusive_flux_reconstructor->getDiffusiveFluxNumberOfGhostCells());
        
        if (d_value_tagger != nullptr)
        {
            d_num_ghosts = hier::IntVector::max(
                d_num_ghosts,
                d_value_tagger->getValueTaggerNumberOfGhostCells());
        }
        
        if (d_gradient_tagger != nullptr)
        {
            d_num_ghosts = hier::IntVector::max(
                d_num_ghosts,
                d_gradient_tagger->getGradientTaggerNumberOfGhostCells());
        }
        
        if (d_multiresolution_tagger != nullptr)
        {
            d_num_ghosts = hier::IntVector::max(
                d_num_ghosts,
                d_multiresolution_tagger->getMultiresolutionTaggerNumberOfGhostCells());
        }
    }
    
    /*
     * Set the number of ghost cells needed in d_flow_model.
     */
    
    d_flow_model->setNumberOfGhostCells(d_num_ghosts);
    
    /*
     * Initialize the face variable of convective flux.
     */
    
    d_variable_convective_flux = boost::shared_ptr<pdat::FaceVariable<double> > (
        new pdat::FaceVariable<double>(dim, "convective flux", d_flow_model->getNumberOfEquations()));
    
    /*
     * Initialize the face variable of diffusive flux.
     */
    
    d_variable_diffusive_flux = boost::shared_ptr<pdat::FaceVariable<double> > (
        new pdat::FaceVariable<double>(dim, "diffusive flux", d_flow_model->getNumberOfEquations()));
    
    /*
     * Initialize the cell variable of source.
     */
    
    d_variable_source = boost::shared_ptr<pdat::CellVariable<double> > (
        new pdat::CellVariable<double>(dim, "source", d_flow_model->getNumberOfEquations()));
}


NavierStokes::~NavierStokes()
{
    t_init.reset();
    t_compute_dt.reset();
    t_compute_hyperbolicfluxes.reset();
    t_advance_steps.reset();
    t_synchronize_hyperbloicfluxes.reset();
    t_setphysbcs.reset();
    t_tagvalue.reset();
    t_taggradient.reset();
    t_tagmultiresolution.reset();
}


void
NavierStokes::registerModelVariables(
    RungeKuttaLevelIntegrator* integrator)
{
    TBOX_ASSERT(integrator != 0);
    
    /*
     * Register the conservative variables of d_flow_model.
     */
    d_flow_model->registerConservativeVariables(integrator);
    
    /*
     * Register the fluxes and sources.
     */
    
    integrator->registerVariable(
        d_variable_convective_flux,
        hier::IntVector::getZero(d_dim),
        RungeKuttaLevelIntegrator::HYP_FLUX,
        d_grid_geometry,
        "CONSERVATIVE_COARSEN",
        "NO_REFINE");
    
    integrator->registerVariable(
        d_variable_diffusive_flux,
        hier::IntVector::getZero(d_dim),
        RungeKuttaLevelIntegrator::HYP_FLUX,
        d_grid_geometry,
        "CONSERVATIVE_COARSEN",
        "NO_REFINE");
    
    integrator->registerVariable(
        d_variable_source,
        hier::IntVector::getZero(d_dim),
        RungeKuttaLevelIntegrator::SOURCE,
        d_grid_geometry,
        "NO_COARSEN",
        "NO_REFINE");
    
    /*
     * Register the temporary variables used in refinement taggers.
     */
    
    if (d_value_tagger != nullptr)
    {
        d_value_tagger->registerValueTaggerVariables(integrator);
    }
    
    if (d_gradient_tagger != nullptr)
    {
        d_gradient_tagger->registerGradientTaggerVariables(integrator);
    }
    
    if (d_multiresolution_tagger != nullptr)
    {
        d_multiresolution_tagger->registerMultiresolutionTaggerVariables(integrator);
    }
    
    /*
     * Set the plotting context.
     */
    setPlotContext(integrator->getPlotContext());
    
    d_flow_model->setPlotContext(integrator->getPlotContext());
    
    /*
     * Register the plotting quantities.
     */
#ifdef HAVE_HDF5
    if (d_visit_writer)
    {
        d_flow_model->registerPlotQuantities(
            d_visit_writer);
        
        if (d_value_tagger != nullptr)
        {
            d_value_tagger->registerPlotQuantities(
                d_visit_writer,
                integrator->getPlotContext());
        }
        
        if (d_gradient_tagger != nullptr)
        {
            d_gradient_tagger->registerPlotQuantities(
                d_visit_writer,
                integrator->getPlotContext());
        }
        
        if (d_multiresolution_tagger != nullptr)
        {
            d_multiresolution_tagger->registerPlotQuantities(
                d_visit_writer,
                integrator->getPlotContext());
        }
    }
    
    if (!d_visit_writer)
    {
        TBOX_WARNING(d_object_name
            << ": registerModelVariables()\n"
            << "VisIt data writer was not registered\n"
            << "Consequently, no plot data will\n"
            << "be written."
            << std::endl);
    }
#endif
}


void
NavierStokes::setupLoadBalancer(
    RungeKuttaLevelIntegrator* integrator,
    mesh::GriddingAlgorithm* gridding_algorithm)
{
    NULL_USE(integrator);
    
    const hier::IntVector& zero_vec = hier::IntVector::getZero(d_dim);
    
    hier::VariableDatabase* vardb = hier::VariableDatabase::getDatabase();
    hier::PatchDataRestartManager* pdrm = hier::PatchDataRestartManager::getManager();
    
    if (d_use_nonuniform_workload && gridding_algorithm)
    {
        boost::shared_ptr<mesh::TreeLoadBalancer> load_balancer(
            boost::dynamic_pointer_cast<mesh::TreeLoadBalancer, mesh::LoadBalanceStrategy>(
                gridding_algorithm->getLoadBalanceStrategy()));
        
        if (load_balancer)
        {
            d_workload_variable.reset(new pdat::CellVariable<double>(
                d_dim,
                "workload_variable",
                1));
            d_workload_data_id = vardb->registerVariableAndContext(
                d_workload_variable,
                vardb->getContext("WORKLOAD"),
                zero_vec);
            load_balancer->setWorkloadPatchDataIndex(d_workload_data_id);
            pdrm->registerPatchDataForRestart(d_workload_data_id);
        }
        else
        {
            TBOX_WARNING(d_object_name << ": "
                                       << "  Unknown load balancer used in gridding algorithm."
                                       << "  Ignoring request for nonuniform load balancing."
                                       << std::endl);
            d_use_nonuniform_workload = false;
        }
    }
    else
    {
        d_use_nonuniform_workload = false;
    }
}


void
NavierStokes::initializeDataOnPatch(
    hier::Patch& patch,
    const double data_time,
    const bool initial_time)
{   
    t_init->start();
    
    d_Navier_Stokes_initial_conditions->initializeDataOnPatch(
        patch,
        data_time,
        initial_time,
        getDataContext());
    
    if (d_use_nonuniform_workload)
    {
        if (!patch.checkAllocated(d_workload_data_id))
        {
            patch.allocatePatchData(d_workload_data_id);
        }
        
        boost::shared_ptr<pdat::CellData<double> > workload_data(
            BOOST_CAST<pdat::CellData<double>, hier::PatchData>(
                patch.getPatchData(d_workload_data_id)));
        TBOX_ASSERT(workload_data);
        workload_data->fillAll(1.0);
    }

    t_init->stop();
}


double
NavierStokes::computeStableDtOnPatch(
    hier::Patch& patch,
    const bool initial_time,
    const double dt_time)
{
    t_compute_dt->start();
    
    double stable_dt;
    
    const boost::shared_ptr<geom::CartesianPatchGeometry> patch_geom(
        BOOST_CAST<geom::CartesianPatchGeometry, hier::PatchGeometry>(
            patch.getPatchGeometry()));
    
#ifdef DEBUG_CHECK_ASSERTIONS
    TBOX_ASSERT(patch_geom);
#endif
    
    const double* dx = patch_geom->getDx();
    
    // Get the dimensions of box that covers the interior of patch.
    const hier::Box interior_box = patch.getBox();
    const hier::IntVector interior_dims = interior_box.numberCells();
    
    double stable_spectral_radius = 0.0;
    
    if (d_dim == tbox::Dimension(1))
    {
        /*
         * Register the patch and maximum wave speed in the flow model and compute the corresponding cell data.
         */
        
        d_flow_model->registerPatchWithDataContext(patch, getDataContext());
        
        std::unordered_map<std::string, hier::IntVector> num_subghosts_of_data;
        num_subghosts_of_data.insert(
            std::pair<std::string, hier::IntVector>(
                "MAX_WAVE_SPEED_X", hier::IntVector::getZero(d_dim)));
        
        d_flow_model->registerDerivedCellVariable(num_subghosts_of_data);
        
        d_flow_model->computeGlobalDerivedCellData();
        
        /*
         * Get the pointer to the maximum wave speed inside the flow model.
         * The numbers of ghost cells and the dimensions of the ghost cell boxes are also determined.
         */
        
        boost::shared_ptr<pdat::CellData<double> > max_wave_speed_x =
            d_flow_model->getGlobalCellData("MAX_WAVE_SPEED_X");
        
        hier::IntVector num_subghosts_max_wave_speed_x = max_wave_speed_x->getGhostCellWidth();
        hier::IntVector subghostcell_dims_max_wave_speed_x = max_wave_speed_x->getGhostBox().numberCells();
        
        double* max_lambda_x = max_wave_speed_x->getPointer(0);
        
        for (int i = 0; i < interior_dims[0]; i++)
        {
            // Compute the linear index.
            const int idx_max_wave_speed_x = i + num_subghosts_max_wave_speed_x[0];
            
            const double spectral_radius = (max_lambda_x[idx_max_wave_speed_x])/dx[0];
            stable_spectral_radius = fmax(stable_spectral_radius, spectral_radius);
        }
        
        /*
         * Unregister the patch and data of all registered derived cell variables in the flow model.
         */
        
        d_flow_model->unregisterPatch();
        
    }
    else if (d_dim == tbox::Dimension(2))
    {
        /*
         * Register the patch and maximum wave speeds in the flow model and compute the corresponding cell data.
         */
        
        d_flow_model->registerPatchWithDataContext(patch, getDataContext());
        
        std::unordered_map<std::string, hier::IntVector> num_subghosts_of_data;
        num_subghosts_of_data.insert(
            std::pair<std::string, hier::IntVector>(
                "MAX_WAVE_SPEED_X", hier::IntVector::getZero(d_dim)));
        num_subghosts_of_data.insert(
            std::pair<std::string, hier::IntVector>(
                "MAX_WAVE_SPEED_Y", hier::IntVector::getZero(d_dim)));
        
        d_flow_model->registerDerivedCellVariable(num_subghosts_of_data);
        
        d_flow_model->computeGlobalDerivedCellData();
        
        /*
         * Get the pointers to the maximum wave speeds inside the flow model.
         * The numbers of ghost cells and the dimensions of the ghost cell boxes are also determined.
         */
        
        boost::shared_ptr<pdat::CellData<double> > max_wave_speed_x =
            d_flow_model->getGlobalCellData("MAX_WAVE_SPEED_X");
        
        boost::shared_ptr<pdat::CellData<double> > max_wave_speed_y =
            d_flow_model->getGlobalCellData("MAX_WAVE_SPEED_Y");
        
        hier::IntVector num_subghosts_max_wave_speed_x = max_wave_speed_x->getGhostCellWidth();
        hier::IntVector subghostcell_dims_max_wave_speed_x = max_wave_speed_x->getGhostBox().numberCells();
        
        hier::IntVector num_subghosts_max_wave_speed_y = max_wave_speed_y->getGhostCellWidth();
        hier::IntVector subghostcell_dims_max_wave_speed_y = max_wave_speed_y->getGhostBox().numberCells();
        
        double* max_lambda_x = max_wave_speed_x->getPointer(0);
        double* max_lambda_y = max_wave_speed_y->getPointer(0);
        
        for (int j = 0; j < interior_dims[1]; j++)
        {
            for (int i = 0; i < interior_dims[0]; i++)
            {
                // Compute the linear indices.
                const int idx_max_wave_speed_x = (i + num_subghosts_max_wave_speed_x[0]) +
                    (j + num_subghosts_max_wave_speed_x[1])*subghostcell_dims_max_wave_speed_x[0];
                
                const int idx_max_wave_speed_y = (i + num_subghosts_max_wave_speed_y[0]) +
                    (j + num_subghosts_max_wave_speed_y[1])*subghostcell_dims_max_wave_speed_y[0];
                
                const double spectral_radius = (max_lambda_x[idx_max_wave_speed_x])/dx[0] +
                    (max_lambda_y[idx_max_wave_speed_y])/dx[1];
                
                stable_spectral_radius = fmax(stable_spectral_radius, spectral_radius);
            }
        }
        
        /*
         * Unregister the patch and data of all registered derived cell variables in the flow model.
         */
        
        d_flow_model->unregisterPatch();
        
    }
    else if (d_dim == tbox::Dimension(3))
    {
        /*
         * Register the patch and maximum wave speeds in the flow model and compute the corresponding cell data.
         */
        d_flow_model->registerPatchWithDataContext(patch, getDataContext());
        
        std::unordered_map<std::string, hier::IntVector> num_subghosts_of_data;
        num_subghosts_of_data.insert(
            std::pair<std::string, hier::IntVector>(
                "MAX_WAVE_SPEED_X", hier::IntVector::getZero(d_dim)));
        num_subghosts_of_data.insert(
            std::pair<std::string, hier::IntVector>(
                "MAX_WAVE_SPEED_Y", hier::IntVector::getZero(d_dim)));
        num_subghosts_of_data.insert(
            std::pair<std::string, hier::IntVector>(
                "MAX_WAVE_SPEED_Z", hier::IntVector::getZero(d_dim)));
        
        d_flow_model->registerDerivedCellVariable(num_subghosts_of_data);
        
        d_flow_model->computeGlobalDerivedCellData();
        
        /*
         * Get the pointers to the maximum wave speeds inside the flow model.
         * The numbers of ghost cells and the dimensions of the ghost cell boxes are also determined.
         */
        
        boost::shared_ptr<pdat::CellData<double> > max_wave_speed_x =
            d_flow_model->getGlobalCellData("MAX_WAVE_SPEED_X");
        
        boost::shared_ptr<pdat::CellData<double> > max_wave_speed_y =
            d_flow_model->getGlobalCellData("MAX_WAVE_SPEED_Y");
        
        boost::shared_ptr<pdat::CellData<double> > max_wave_speed_z =
            d_flow_model->getGlobalCellData("MAX_WAVE_SPEED_Z");
        
        hier::IntVector num_subghosts_max_wave_speed_x = max_wave_speed_x->getGhostCellWidth();
        hier::IntVector subghostcell_dims_max_wave_speed_x = max_wave_speed_x->getGhostBox().numberCells();
        
        hier::IntVector num_subghosts_max_wave_speed_y = max_wave_speed_y->getGhostCellWidth();
        hier::IntVector subghostcell_dims_max_wave_speed_y = max_wave_speed_y->getGhostBox().numberCells();
        
        hier::IntVector num_subghosts_max_wave_speed_z = max_wave_speed_z->getGhostCellWidth();
        hier::IntVector subghostcell_dims_max_wave_speed_z = max_wave_speed_z->getGhostBox().numberCells();
        
        double* max_lambda_x = max_wave_speed_x->getPointer(0);
        double* max_lambda_y = max_wave_speed_y->getPointer(0);
        double* max_lambda_z = max_wave_speed_z->getPointer(0);
        
        for (int k = 0; k < interior_dims[2]; k++)
        {
            for (int j = 0; j < interior_dims[1]; j++)
            {
                for (int i = 0; i < interior_dims[0]; i++)
                {
                    // Compute the linear indices.
                    const int idx_max_wave_speed_x = (i + num_subghosts_max_wave_speed_x[0]) +
                        (j + num_subghosts_max_wave_speed_x[1])*subghostcell_dims_max_wave_speed_x[0] +
                        (k + num_subghosts_max_wave_speed_x[2])*subghostcell_dims_max_wave_speed_x[0]*
                            subghostcell_dims_max_wave_speed_x[1];
                    
                    const int idx_max_wave_speed_y = (i + num_subghosts_max_wave_speed_y[0]) +
                        (j + num_subghosts_max_wave_speed_y[1])*subghostcell_dims_max_wave_speed_y[0] +
                        (k + num_subghosts_max_wave_speed_y[2])*subghostcell_dims_max_wave_speed_y[0]*
                            subghostcell_dims_max_wave_speed_y[1];
                    
                    const int idx_max_wave_speed_z = (i + num_subghosts_max_wave_speed_z[0]) +
                        (j + num_subghosts_max_wave_speed_z[1])*subghostcell_dims_max_wave_speed_z[0] +
                        (k + num_subghosts_max_wave_speed_z[2])*subghostcell_dims_max_wave_speed_z[0]*
                            subghostcell_dims_max_wave_speed_z[1];
                    
                    const double spectral_radius = (max_lambda_x[idx_max_wave_speed_x])/dx[0] +
                        (max_lambda_y[idx_max_wave_speed_y])/dx[1] +
                        (max_lambda_z[idx_max_wave_speed_z])/dx[2];
                    
                    stable_spectral_radius = fmax(stable_spectral_radius, spectral_radius);
                }
            }
        }
        
        /*
         * Unregister the patch and data of all registered derived cell variables in the flow model.
         */
        
        d_flow_model->unregisterPatch();
    }
    
    stable_dt = 1.0/stable_spectral_radius;
    
    t_compute_dt->stop();
    
    return stable_dt;
}


void
NavierStokes::computeHyperbolicFluxesAndSourcesOnPatch(
    hier::Patch& patch,
    const double time,
    const double dt,
    const int RK_step_number)
{
    NULL_USE(time);
    
    t_compute_hyperbolicfluxes->start();
    
    /*
     * Set zero for the source.
     */
    
    boost::shared_ptr<pdat::CellData<double> > data_source(
        BOOST_CAST<pdat::CellData<double>, hier::PatchData>(
            patch.getPatchData(d_variable_source, getDataContext())));
    
    data_source->fillAll(0.0);
    
    /*
     * Compute the fluxes and sources.
     */
    
    d_convective_flux_reconstructor->computeConvectiveFluxesAndSources(
        patch,
        time,
        dt,
        RK_step_number,
        d_variable_convective_flux,
        d_variable_source,
        getDataContext());
    
    d_diffusive_flux_reconstructor->computeDiffusiveFluxes(
        patch,
        time,
        dt,
        RK_step_number,
        d_variable_diffusive_flux,
        getDataContext());
    
    t_compute_hyperbolicfluxes->stop();
}


void
NavierStokes::advanceSingleStep(
    hier::Patch& patch,
    const double time,
    const double dt,
    const std::vector<double>& alpha,
    const std::vector<double>& beta,
    const std::vector<double>& gamma,
    const std::vector<boost::shared_ptr<hier::VariableContext> >& intermediate_context)
{
    NULL_USE(time);
    NULL_USE(dt);
    
    t_advance_steps->start();
    
    const boost::shared_ptr<geom::CartesianPatchGeometry> patch_geom(
        BOOST_CAST<geom::CartesianPatchGeometry, hier::PatchGeometry>(
            patch.getPatchGeometry()));
    
#ifdef DEBUG_CHECK_ASSERTIONS
    TBOX_ASSERT(patch_geom);
#endif
    
    const double* dx = patch_geom->getDx();
    
    // Get the dimensions of box that covers the interior of patch.
    const hier::Box interior_box = patch.getBox();
    const hier::IntVector interior_dims = interior_box.numberCells();
    
    /*
     * Get a vector of pointers to the conservative variables for the current data context (SCRATCH).
     * The numbers of ghost cells and the dimensions of the ghost cell boxes are also determined.
     */
    
    d_flow_model->registerPatchWithDataContext(patch, getDataContext());
    
    std::vector<boost::shared_ptr<pdat::CellData<double> > > conservative_variables =
        d_flow_model->getGlobalCellDataConservativeVariables();
    
    std::vector<hier::IntVector> num_ghosts_conservative_var;
    num_ghosts_conservative_var.reserve(d_flow_model->getNumberOfEquations());
    
    std::vector<hier::IntVector> ghostcell_dims_conservative_var;
    ghostcell_dims_conservative_var.reserve(d_flow_model->getNumberOfEquations());
    
    std::vector<double*> Q;
    Q.reserve(d_flow_model->getNumberOfEquations());
    
    int count_eqn = 0;
    
    for (int vi = 0; vi < static_cast<int>(conservative_variables.size()); vi++)
    {
        int depth = conservative_variables[vi]->getDepth();
        
        for (int di = 0; di < depth; di++)
        {
            // If the last element of the conservative variable vector is not in the system of equations, ignore it.
            if (count_eqn >= d_flow_model->getNumberOfEquations())
                break;
            
            Q.push_back(conservative_variables[vi]->getPointer(di));
            num_ghosts_conservative_var.push_back(conservative_variables[vi]->getGhostCellWidth());
            ghostcell_dims_conservative_var.push_back(conservative_variables[vi]->getGhostBox().numberCells());
            
            count_eqn++;
        }
    }
    
    d_flow_model->fillZeroGlobalCellDataConservativeVariables();
    
    // Unregister the patch.
    d_flow_model->unregisterPatch();
    
    /*
     * Use alpha, beta and gamma values to update the time-dependent solution,
     * flux and source
     */
    
    boost::shared_ptr<pdat::FaceData<double> > convective_flux(
        BOOST_CAST<pdat::FaceData<double>, hier::PatchData>(
            patch.getPatchData(d_variable_convective_flux, getDataContext())));
    
    boost::shared_ptr<pdat::FaceData<double> > diffusive_flux(
        BOOST_CAST<pdat::FaceData<double>, hier::PatchData>(
            patch.getPatchData(d_variable_diffusive_flux, getDataContext())));
    
    boost::shared_ptr<pdat::CellData<double> > source(
        BOOST_CAST<pdat::CellData<double>, hier::PatchData>(
            patch.getPatchData(d_variable_source, getDataContext())));
    
#ifdef DEBUG_CHECK_ASSERTIONS
    TBOX_ASSERT(convective_flux);
    TBOX_ASSERT(diffusive_flux);
    TBOX_ASSERT(source);
    
    TBOX_ASSERT(convective_flux->getGhostCellWidth() == hier::IntVector::getZero(d_dim));
    TBOX_ASSERT(diffusive_flux->getGhostCellWidth() == hier::IntVector::getZero(d_dim));
    TBOX_ASSERT(source->getGhostCellWidth() == hier::IntVector::getZero(d_dim));
#endif
    
    int num_coeffs = static_cast<int>(alpha.size());
    
    for (int n = 0; n < num_coeffs; n++)
    {
        boost::shared_ptr<pdat::FaceData<double> > convective_flux_intermediate(
            BOOST_CAST<pdat::FaceData<double>, hier::PatchData>(
                    patch.getPatchData(d_variable_convective_flux, intermediate_context[n])));
        
        boost::shared_ptr<pdat::FaceData<double> > diffusive_flux_intermediate(
            BOOST_CAST<pdat::FaceData<double>, hier::PatchData>(
                    patch.getPatchData(d_variable_diffusive_flux, intermediate_context[n])));
        
        boost::shared_ptr<pdat::CellData<double> > source_intermediate(
            BOOST_CAST<pdat::CellData<double>, hier::PatchData>(
                patch.getPatchData(d_variable_source, intermediate_context[n])));
        
#ifdef DEBUG_CHECK_ASSERTIONS
        TBOX_ASSERT(convective_flux_intermediate);
        TBOX_ASSERT(diffusive_flux_intermediate);
        TBOX_ASSERT(source_intermediate);
        
        TBOX_ASSERT(convective_flux_intermediate->getGhostCellWidth() == hier::IntVector::getZero(d_dim));
        TBOX_ASSERT(diffusive_flux_intermediate->getGhostCellWidth() == hier::IntVector::getZero(d_dim));
        TBOX_ASSERT(source_intermediate->getGhostCellWidth() == hier::IntVector::getZero(d_dim));
#endif
        
        /*
         * Create a vector pointers to the time-dependent variables for the
         * current intermediate data context.
         */
        
        d_flow_model->registerPatchWithDataContext(patch, intermediate_context[n]);
        
        std::vector<boost::shared_ptr<pdat::CellData<double> > > conservative_variables_intermediate =
            d_flow_model->getGlobalCellDataConservativeVariables();
        
        std::vector<double*> Q_intermediate;
        Q_intermediate.reserve(d_flow_model->getNumberOfEquations());
        
        count_eqn = 0;
        
        for (int vi = 0; vi < static_cast<int>(conservative_variables_intermediate.size()); vi++)
        {
            int depth = conservative_variables_intermediate[vi]->getDepth();
            
            for (int di = 0; di < depth; di++)
            {
                // If the last element of the conservative variable vector is not in the system of equations, ignore it.
                if (count_eqn >= d_flow_model->getNumberOfEquations())
                    break;
                
                Q_intermediate.push_back(conservative_variables_intermediate[vi]->getPointer(di));
                
                count_eqn++;
            }
        }
        
        // Unregister the patch.
        d_flow_model->unregisterPatch();
        
        if (d_dim == tbox::Dimension(1))
        {
            if (alpha[n] != 0.0)
            {
                for (int i = 0; i < interior_dims[0]; i++)
                {
                    for (int ei = 0; ei < d_flow_model->getNumberOfEquations(); ei++)
                    {
                        // Compute linear index of conservative variable data.
                        int idx_cell = i + num_ghosts_conservative_var[ei][0];
                        
                        Q[ei][idx_cell] += alpha[n]*Q_intermediate[ei][idx_cell];
                    }
                }
            }
            
            if (beta[n] != 0.0)
            {
                for (int i = 0; i < interior_dims[0]; i++)
                {
                    // Compute linear indices of  flux and source.
                    int idx_source = i;
                    int idx_flux_x = i + 1;
                    
                    for (int ei = 0; ei < d_flow_model->getNumberOfEquations(); ei++)
                    {
                        // Compute linear index of conservative variable data.
                        int idx_cell = i + num_ghosts_conservative_var[ei][0];
                        
                        double* F_c_x_intermediate = convective_flux_intermediate->getPointer(0, ei);
                        double* F_d_x_intermediate = diffusive_flux_intermediate->getPointer(0, ei);
                        double* S_intermediate = source_intermediate->getPointer(ei);
                        
                        Q[ei][idx_cell] += beta[n]*
                            (-(F_c_x_intermediate[idx_flux_x] - F_c_x_intermediate[idx_flux_x - 1] +
                               F_d_x_intermediate[idx_flux_x] - F_d_x_intermediate[idx_flux_x - 1])/dx[0] +
                            S_intermediate[idx_source]);
                    }
                }
            }
                
            if (gamma[n] != 0.0)
            {
                // Accumulate the flux in the x direction.
                for (int i = 0; i < interior_dims[0] + 1; i++)
                {
                    int idx_flux_x = i;
                    
                    for (int ei = 0; ei < d_flow_model->getNumberOfEquations(); ei++)
                    {
                        double* F_c_x              = convective_flux->getPointer(0, ei);
                        double* F_c_x_intermediate = convective_flux_intermediate->getPointer(0, ei);
                        
                        double* F_d_x              = diffusive_flux->getPointer(0, ei);
                        double* F_d_x_intermediate = diffusive_flux_intermediate->getPointer(0, ei);
                        
                        F_c_x[idx_flux_x] += gamma[n]*F_c_x_intermediate[idx_flux_x];
                        F_d_x[idx_flux_x] += gamma[n]*F_d_x_intermediate[idx_flux_x];
                    }                        
                }
                
                // Accumulate the source.
                for (int i = 0; i < interior_dims[0]; i++)
                {
                    int idx_cell = i;
                    
                    for (int ei = 0; ei < d_flow_model->getNumberOfEquations(); ei++)
                    {
                        double* S              = source->getPointer(ei);
                        double* S_intermediate = source_intermediate->getPointer(ei);
                        
                        S[idx_cell] += gamma[n]*S_intermediate[idx_cell];
                    }
                }
            } // if (gamma[n] != 0.0)
        } // if (d_dim == tbox::Dimension(1))
        else if (d_dim == tbox::Dimension(2))
        {
            if (alpha[n] != 0.0)
            {
                for (int j = 0; j < interior_dims[1]; j++)
                {
                    for (int i = 0; i < interior_dims[0]; i++)
                    {
                        for (int ei = 0; ei < d_flow_model->getNumberOfEquations(); ei++)
                        {
                            // Compute linear index of conservative data.
                            int idx_cell = (i + num_ghosts_conservative_var[ei][0]) +
                                (j + num_ghosts_conservative_var[ei][1])*ghostcell_dims_conservative_var[ei][0];
                            
                            Q[ei][idx_cell] += alpha[n]*Q_intermediate[ei][idx_cell];
                        }
                    }
                }
            }
            
            if (beta[n] != 0.0)
            {
                for (int j = 0; j < interior_dims[1]; j++)
                {
                    for (int i = 0; i < interior_dims[0]; i++)
                    {
                        // Compute linear indices of flux and source.
                        int idx_source = i + j*interior_dims[0];
                        int idx_flux_x = (i + 1) + j*(interior_dims[0] + 1);
                        int idx_flux_y = (j + 1) + i*(interior_dims[1] + 1);
                        
                        for (int ei = 0; ei < d_flow_model->getNumberOfEquations(); ei++)
                        {
                            // Compute linear index of conservative variable data.
                            int idx_cell = (i + num_ghosts_conservative_var[ei][0]) +
                                (j + num_ghosts_conservative_var[ei][1])*ghostcell_dims_conservative_var[ei][0];
                            
                            double* F_c_x_intermediate = convective_flux_intermediate->getPointer(0, ei);
                            double* F_c_y_intermediate = convective_flux_intermediate->getPointer(1, ei);
                            double* F_d_x_intermediate = diffusive_flux_intermediate->getPointer(0, ei);
                            double* F_d_y_intermediate = diffusive_flux_intermediate->getPointer(1, ei);
                            double* S_intermediate = source_intermediate->getPointer(ei);
                            
                            Q[ei][idx_cell] += beta[n]*
                                (-(F_c_x_intermediate[idx_flux_x] - F_c_x_intermediate[idx_flux_x - 1] +
                                   F_d_x_intermediate[idx_flux_x] - F_d_x_intermediate[idx_flux_x - 1])/dx[0] -
                                (F_c_y_intermediate[idx_flux_y] - F_c_y_intermediate[idx_flux_y - 1] +
                                 F_d_y_intermediate[idx_flux_y] - F_d_y_intermediate[idx_flux_y - 1])/dx[1] +
                                S_intermediate[idx_source]);
                        }
                    }
                }
            }
            
            if (gamma[n] != 0.0)
            {
                // Accumulate the flux in the x direction.
                for (int j = 0; j < interior_dims[1]; j++)
                {
                    for (int i = 0; i < interior_dims[0] + 1; i++)
                    {
                        int idx_flux_x = i + j*(interior_dims[0] + 1);
                        
                        for (int ei = 0; ei < d_flow_model->getNumberOfEquations(); ei++)
                        {
                            double* F_c_x              = convective_flux->getPointer(0, ei);
                            double* F_c_x_intermediate = convective_flux_intermediate->getPointer(0, ei);
                            
                            double* F_d_x              = diffusive_flux->getPointer(0, ei);
                            double* F_d_x_intermediate = diffusive_flux_intermediate->getPointer(0, ei);
                            
                            F_c_x[idx_flux_x] += gamma[n]*F_c_x_intermediate[idx_flux_x];
                            F_d_x[idx_flux_x] += gamma[n]*F_d_x_intermediate[idx_flux_x];
                        }                        
                    }
                }
                
                // Accumulate the flux in the y direction.
                for (int i = 0; i < interior_dims[0]; i++)
                {
                    for (int j = 0; j < interior_dims[1] + 1; j++)
                    {
                        int idx_flux_y = j + i*(interior_dims[1] + 1);
                        
                        for (int ei = 0; ei < d_flow_model->getNumberOfEquations(); ei++)
                        {
                            double* F_c_y              = convective_flux->getPointer(1, ei);
                            double* F_c_y_intermediate = convective_flux_intermediate->getPointer(1, ei);
                            
                            double* F_d_y              = diffusive_flux->getPointer(1, ei);
                            double* F_d_y_intermediate = diffusive_flux_intermediate->getPointer(1, ei);
                            
                            F_c_y[idx_flux_y] += gamma[n]*F_c_y_intermediate[idx_flux_y];
                            F_d_y[idx_flux_y] += gamma[n]*F_d_y_intermediate[idx_flux_y];
                        }
                    }
                }
                
                // Accumulate the source.
                for (int j = 0; j < interior_dims[1]; j++)
                {
                    for (int i = 0; i < interior_dims[0]; i++)
                    {
                        int idx_cell = i + j*interior_dims[0];
                        
                        for (int ei = 0; ei < d_flow_model->getNumberOfEquations(); ei++)
                        {
                            double* S              = source->getPointer(ei);
                            double* S_intermediate = source_intermediate->getPointer(ei);
                            
                            S[idx_cell] += gamma[n]*S_intermediate[idx_cell];
                        }
                    }
                }
            } // if (gamma[n] != 0.0)
        } // if (d_dim == tbox::Dimension(2))
        else if (d_dim == tbox::Dimension(3))
        {
            if (alpha[n] != 0.0)
            {
                for (int k = 0; k < interior_dims[2]; k++)
                {
                    for (int j = 0; j < interior_dims[1]; j++)
                    {
                        for (int i = 0; i < interior_dims[0]; i++)
                        {
                            for (int ei = 0; ei < d_flow_model->getNumberOfEquations(); ei++)
                            {
                                // Compute linear index of conservative variable data.
                                int idx_cell = (i + num_ghosts_conservative_var[ei][0]) +
                                    (j + num_ghosts_conservative_var[ei][1])*ghostcell_dims_conservative_var[ei][0] +
                                    (k + num_ghosts_conservative_var[ei][2])*ghostcell_dims_conservative_var[ei][0]*
                                        ghostcell_dims_conservative_var[ei][1];
                                
                                Q[ei][idx_cell] += alpha[n]*Q_intermediate[ei][idx_cell];
                            }
                        }
                    }
                }
            }
            
            if (beta[n] != 0.0)
            {
                for (int k = 0; k < interior_dims[2]; k++)
                {
                    for (int j = 0; j < interior_dims[1]; j++)
                    {
                        for (int i = 0; i < interior_dims[0]; i++)
                        {
                            // Compute linear indices of flux and source.
                            int idx_source = i +
                                j*interior_dims[0] +
                                k*interior_dims[0]*interior_dims[1];
                            
                            int idx_flux_x = (i + 1) +
                                j*(interior_dims[0] + 1) +
                                k*(interior_dims[0] + 1)*interior_dims[1];
                            
                            int idx_flux_y = (j + 1) +
                                k*(interior_dims[1] + 1) +
                                i*(interior_dims[1] + 1)*interior_dims[2];
                            
                            int idx_flux_z = (k + 1) +
                                i*(interior_dims[2] + 1) +
                                j*(interior_dims[2] + 1)*interior_dims[0];
                            
                            for (int ei = 0; ei < d_flow_model->getNumberOfEquations(); ei++)
                            {
                                // Compute linear index of conservative data.
                                int idx_cell = (i + num_ghosts_conservative_var[ei][0]) +
                                    (j + num_ghosts_conservative_var[ei][1])*ghostcell_dims_conservative_var[ei][0] +
                                    (k + num_ghosts_conservative_var[ei][2])*ghostcell_dims_conservative_var[ei][0]*
                                        ghostcell_dims_conservative_var[ei][1];
                                
                                double* F_c_x_intermediate = convective_flux_intermediate->getPointer(0, ei);
                                double* F_c_y_intermediate = convective_flux_intermediate->getPointer(1, ei);
                                double* F_c_z_intermediate = convective_flux_intermediate->getPointer(2, ei);
                                double* F_d_x_intermediate = diffusive_flux_intermediate->getPointer(0, ei);
                                double* F_d_y_intermediate = diffusive_flux_intermediate->getPointer(1, ei);
                                double* F_d_z_intermediate = diffusive_flux_intermediate->getPointer(2, ei);
                                double* S_intermediate = source_intermediate->getPointer(ei);
                                
                                Q[ei][idx_cell] += beta[n]*
                                    (-(F_c_x_intermediate[idx_flux_x] - F_c_x_intermediate[idx_flux_x - 1] +
                                       F_d_x_intermediate[idx_flux_x] - F_d_x_intermediate[idx_flux_x - 1])/dx[0] -
                                    (F_c_y_intermediate[idx_flux_y] - F_c_y_intermediate[idx_flux_y - 1] +
                                     F_d_y_intermediate[idx_flux_y] - F_d_y_intermediate[idx_flux_y - 1])/dx[1] -
                                    (F_c_z_intermediate[idx_flux_z] - F_c_z_intermediate[idx_flux_z - 1] +
                                     F_d_z_intermediate[idx_flux_z] - F_d_z_intermediate[idx_flux_z - 1])/dx[2] +
                                    S_intermediate[idx_source]);
                            }
                        }
                    }
                }
            }
            
            if (gamma[n] != 0.0)
            {
                // Accumulate the flux in the x direction.
                for (int k = 0; k < interior_dims[2]; k++)
                {
                    for (int j = 0; j < interior_dims[1]; j++)
                    {
                        for (int i = 0; i < interior_dims[0] + 1; i++)
                        {
                            int idx_flux_x = i +
                                j*(interior_dims[0] + 1) +
                                k*(interior_dims[0] + 1)*interior_dims[1];
                            
                            for (int ei = 0; ei < d_flow_model->getNumberOfEquations(); ei++)
                            {
                                double* F_c_x              = convective_flux->getPointer(0, ei);
                                double* F_c_x_intermediate = convective_flux_intermediate->getPointer(0, ei);
                                
                                double* F_d_x              = diffusive_flux->getPointer(0, ei);
                                double* F_d_x_intermediate = diffusive_flux_intermediate->getPointer(0, ei);
                                
                                F_c_x[idx_flux_x] += gamma[n]*F_c_x_intermediate[idx_flux_x];
                                F_d_x[idx_flux_x] += gamma[n]*F_d_x_intermediate[idx_flux_x];
                            }                        
                        }
                    }
                }
                
                // Accumulate the flux in the y direction.
                for (int i = 0; i < interior_dims[0]; i++)
                {
                    for (int k = 0; k < interior_dims[2]; k++)
                    {
                        for (int j = 0; j < interior_dims[1] + 1; j++)
                        {
                            int idx_flux_y = j +
                                k*(interior_dims[1] + 1) +
                                i*(interior_dims[1] + 1)*interior_dims[2];
                            
                            for (int ei = 0; ei < d_flow_model->getNumberOfEquations(); ei++)
                            {
                                double* F_c_y              = convective_flux->getPointer(1, ei);
                                double* F_c_y_intermediate = convective_flux_intermediate->getPointer(1, ei);
                                
                                double* F_d_y              = diffusive_flux->getPointer(1, ei);
                                double* F_d_y_intermediate = diffusive_flux_intermediate->getPointer(1, ei);
                                
                                F_c_y[idx_flux_y] += gamma[n]*F_c_y_intermediate[idx_flux_y];
                                F_d_y[idx_flux_y] += gamma[n]*F_d_y_intermediate[idx_flux_y];
                            }
                        }
                    }
                }
                
                // Accumulate the flux in the z direction.
                for (int j = 0; j < interior_dims[1]; j++)
                {
                    for (int i = 0; i < interior_dims[0]; i++)
                    {
                        for (int k = 0; k < interior_dims[2] + 1; k++)
                        {
                            int idx_flux_z = k +
                                i*(interior_dims[2] + 1) +
                                j*(interior_dims[2] + 1)*interior_dims[0];
                            
                            for (int ei = 0; ei < d_flow_model->getNumberOfEquations(); ei++)
                            {
                                double* F_c_z              = convective_flux->getPointer(2, ei);
                                double* F_c_z_intermediate = convective_flux_intermediate->getPointer(2, ei);
                                
                                double* F_d_z              = diffusive_flux->getPointer(2, ei);
                                double* F_d_z_intermediate = diffusive_flux_intermediate->getPointer(2, ei);
                                
                                F_c_z[idx_flux_z] += gamma[n]*F_c_z_intermediate[idx_flux_z];
                                F_d_z[idx_flux_z] += gamma[n]*F_d_z_intermediate[idx_flux_z];
                            }
                        }
                    }
                }
                
                // Accumulate the source.
                for (int k = 0; k < interior_dims[2]; k++)
                {
                    for (int j = 0; j < interior_dims[1]; j++)
                    {
                        for (int i = 0; i < interior_dims[0]; i++)
                        {
                            int idx_cell = i +
                                j*interior_dims[0] +
                                k*interior_dims[0]*interior_dims[1];
                            
                            for (int ei = 0; ei < d_flow_model->getNumberOfEquations(); ei++)
                            {
                                double* S              = source->getPointer(ei);
                                double* S_intermediate = source_intermediate->getPointer(ei);
                                
                                S[idx_cell] += gamma[n]*S_intermediate[idx_cell];
                            }
                        }
                    }
                }
            } // if (gamma[n] != 0.0)
        } // if (d_dim == tbox::Dimension(3))
        
        /*
         * Update the conservative variables.
         */
        
        if (beta[n] != 0.0)
        {
            d_flow_model->registerPatchWithDataContext(patch, getDataContext());
            
            d_flow_model->updateGlobalCellDataConservativeVariables();
            
            d_flow_model->unregisterPatch();
        }
    }
    
    t_advance_steps->stop();
}


void
NavierStokes::synchronizeHyperbolicFluxes(
    hier::Patch& patch,
    const double time,
    const double dt)
{
    NULL_USE(time);
    NULL_USE(dt);
    
    t_synchronize_hyperbloicfluxes->start();
    
    const boost::shared_ptr<geom::CartesianPatchGeometry> patch_geom(
        BOOST_CAST<geom::CartesianPatchGeometry, hier::PatchGeometry>(
            patch.getPatchGeometry()));
    
#ifdef DEBUG_CHECK_ASSERTIONS
    TBOX_ASSERT(patch_geom);
#endif
    
    const double* dx = patch_geom->getDx();
    
    // Get the dimensions of box that covers the interior of patch.
    const hier::Box interior_box = patch.getBox();
    const hier::IntVector interior_dims = interior_box.numberCells();
    
    /*
     * Get a vector of pointers to the conservative variables for the current data context (SCRATCH).
     * The numbers of ghost cells and the dimensions of the ghost cell boxes are also determined.
     */
    
    d_flow_model->registerPatchWithDataContext(patch, getDataContext());
    
    std::vector<boost::shared_ptr<pdat::CellData<double> > > conservative_variables =
        d_flow_model->getGlobalCellDataConservativeVariables();
    
    std::vector<hier::IntVector> num_ghosts_conservative_var;
    num_ghosts_conservative_var.reserve(d_flow_model->getNumberOfEquations());
    
    std::vector<hier::IntVector> ghostcell_dims_conservative_var;
    ghostcell_dims_conservative_var.reserve(d_flow_model->getNumberOfEquations());
    
    std::vector<double*> Q;
    Q.reserve(d_flow_model->getNumberOfEquations());
    
    int count_eqn = 0;
    
    for (int vi = 0; vi < static_cast<int>(conservative_variables.size()); vi++)
    {
        int depth = conservative_variables[vi]->getDepth();
        
        for (int di = 0; di < depth; di++)
        {
            // If the last element of the conservative variable vector is not in the system of equations, ignore it.
            if (count_eqn >= d_flow_model->getNumberOfEquations())
                break;
            
            Q.push_back(conservative_variables[vi]->getPointer(di));
            num_ghosts_conservative_var.push_back(conservative_variables[vi]->getGhostCellWidth());
            ghostcell_dims_conservative_var.push_back(conservative_variables[vi]->getGhostBox().numberCells());
            
            count_eqn++;
        }
    }
    
    // Unregister the patch.
    d_flow_model->unregisterPatch();
    
    boost::shared_ptr<pdat::FaceData<double> > convective_flux(
        BOOST_CAST<pdat::FaceData<double>, hier::PatchData>(
            patch.getPatchData(d_variable_convective_flux, getDataContext())));
    
    boost::shared_ptr<pdat::FaceData<double> > diffusive_flux(
        BOOST_CAST<pdat::FaceData<double>, hier::PatchData>(
            patch.getPatchData(d_variable_diffusive_flux, getDataContext())));
    
    boost::shared_ptr<pdat::CellData<double> > source(
        BOOST_CAST<pdat::CellData<double>, hier::PatchData>(
            patch.getPatchData(d_variable_source, getDataContext())));
    
#ifdef DEBUG_CHECK_ASSERTIONS
    TBOX_ASSERT(convective_flux);
    TBOX_ASSERT(diffusive_flux);
    TBOX_ASSERT(source);
    
    TBOX_ASSERT(convective_flux->getGhostCellWidth() == hier::IntVector::getZero(d_dim));
    TBOX_ASSERT(diffusive_flux->getGhostCellWidth() == hier::IntVector::getZero(d_dim));
    TBOX_ASSERT(source->getGhostCellWidth() == hier::IntVector::getZero(d_dim));
#endif
    
    if (d_dim == tbox::Dimension(1))
    {
        for (int i = 0; i < interior_dims[0]; i++)
        {
            // Compute linear indices of  flux and source.
            int idx_source = i;
            int idx_flux_x = i + 1;
            
            for (int ei = 0; ei < d_flow_model->getNumberOfEquations(); ei++)
            {
                // Compute linear index of conservative variable data.
                int idx_cell = i + num_ghosts_conservative_var[ei][0];
                
                double *F_c_x = convective_flux->getPointer(0, ei);
                double *F_d_x = diffusive_flux->getPointer(0, ei);
                double *S     = source->getPointer(ei);
                
                Q[ei][idx_cell] += (-(F_c_x[idx_flux_x] - F_c_x[idx_flux_x - 1] +
                                      F_d_x[idx_flux_x] - F_d_x[idx_flux_x - 1])/dx[0] +
                    S[idx_source]);
            }
        }
    }
    else if (d_dim == tbox::Dimension(2))
    {
        for (int j = 0; j < interior_dims[1]; j++)
        {
            for (int i = 0; i < interior_dims[0]; i++)
            {
                // Compute linear indices of flux and source.
                int idx_source = i + j*interior_dims[0];
                int idx_flux_x = (i + 1) + j*(interior_dims[0] + 1);
                int idx_flux_y = (j + 1) + i*(interior_dims[1] + 1);
                
                for (int ei = 0; ei < d_flow_model->getNumberOfEquations(); ei++)
                {
                    // Compute linear index of conservative variable data.
                    int idx_cell = (i + num_ghosts_conservative_var[ei][0]) +
                        (j + num_ghosts_conservative_var[ei][1])*ghostcell_dims_conservative_var[ei][0];
                    
                    double *F_c_x = convective_flux->getPointer(0, ei);
                    double *F_c_y = convective_flux->getPointer(1, ei);
                    double *F_d_x = diffusive_flux->getPointer(0, ei);
                    double *F_d_y = diffusive_flux->getPointer(1, ei);
                    double *S     = source->getPointer(ei);
                    
                    Q[ei][idx_cell] += (-(F_c_x[idx_flux_x] - F_c_x[idx_flux_x - 1] +
                                          F_d_x[idx_flux_x] - F_d_x[idx_flux_x - 1])/dx[0] -
                        (F_c_y[idx_flux_y] - F_c_y[idx_flux_y - 1] +
                         F_d_y[idx_flux_y] - F_d_y[idx_flux_y - 1])/dx[1] +
                        S[idx_source]);
                }
            }
        }
    }
    else if (d_dim == tbox::Dimension(3))
    {
        for (int k = 0; k < interior_dims[2]; k++)
        {
            for (int j = 0; j < interior_dims[1]; j++)
            {
                for (int i = 0; i < interior_dims[0]; i++)
                {
                    // Compute linear indices of flux and source.
                    int idx_source = i +
                        j*interior_dims[0] +
                        k*interior_dims[0]*interior_dims[1];
                    
                    int idx_flux_x = (i + 1) +
                        j*(interior_dims[0] + 1) +
                        k*(interior_dims[0] + 1)*interior_dims[1];
                    
                    int idx_flux_y = (j + 1) +
                        k*(interior_dims[1] + 1) +
                        i*(interior_dims[1] + 1)*interior_dims[2];
                    
                    int idx_flux_z = (k + 1) +
                        i*(interior_dims[2] + 1) +
                        j*(interior_dims[2] + 1)*interior_dims[0];
                    
                    for (int ei = 0; ei < d_flow_model->getNumberOfEquations(); ei++)
                    {
                        // Compute linear index of conservative data.
                        int idx_cell = (i + num_ghosts_conservative_var[ei][0]) +
                            (j + num_ghosts_conservative_var[ei][1])*ghostcell_dims_conservative_var[ei][0] +
                            (k + num_ghosts_conservative_var[ei][2])*ghostcell_dims_conservative_var[ei][0]*
                                ghostcell_dims_conservative_var[ei][1];
                        
                        double *F_c_x = convective_flux->getPointer(0, ei);
                        double *F_c_y = convective_flux->getPointer(1, ei);
                        double *F_c_z = convective_flux->getPointer(2, ei);
                        double *F_d_x = diffusive_flux->getPointer(0, ei);
                        double *F_d_y = diffusive_flux->getPointer(1, ei);
                        double *F_d_z = diffusive_flux->getPointer(2, ei);
                        double *S     = source->getPointer(ei);
                        
                        Q[ei][idx_cell] += (-(F_c_x[idx_flux_x] - F_c_x[idx_flux_x - 1] +
                                              F_d_x[idx_flux_x] - F_d_x[idx_flux_x - 1])/dx[0] -
                            (F_c_y[idx_flux_y] - F_c_y[idx_flux_y - 1] +
                             F_d_y[idx_flux_y] - F_d_y[idx_flux_y - 1])/dx[1] -
                            (F_c_z[idx_flux_z] - F_c_z[idx_flux_z - 1] +
                             F_d_z[idx_flux_z] - F_d_z[idx_flux_z - 1])/dx[2] +
                            S[idx_source]);
                    }
                }
            }
        }
    }
    
    /*
     * Update the conservative variables.
     */
    
    d_flow_model->registerPatchWithDataContext(patch, getDataContext());
    
    d_flow_model->updateGlobalCellDataConservativeVariables();
    
    d_flow_model->unregisterPatch();
    
    t_synchronize_hyperbloicfluxes->stop();
}


/*
 * Preprocess before tagging cells using value detector.
 */
void
NavierStokes::preprocessTagValueDetectorCells(
   const boost::shared_ptr<hier::PatchHierarchy>& patch_hierarchy,
   const int level_number,
   const double regrid_time,
   const bool initial_error,
   const bool uses_gradient_detector_too,
   const bool uses_multiresolution_detector_too,
   const bool uses_integral_detector_too,
   const bool uses_richardson_extrapolation_too)
{
    NULL_USE(regrid_time);
    NULL_USE(initial_error);
    NULL_USE(uses_gradient_detector_too);
    NULL_USE(uses_multiresolution_detector_too);
    NULL_USE(uses_integral_detector_too);
    NULL_USE(uses_richardson_extrapolation_too);
    
    if (d_value_tagger != nullptr)
    {
        boost::shared_ptr<hier::PatchLevel> level(
            patch_hierarchy->getPatchLevel(level_number));
        
        for (hier::PatchLevel::iterator ip(level->begin());
             ip != level->end();
             ip++)
        {
            const boost::shared_ptr<hier::Patch>& patch = *ip;
            
            d_value_tagger->computeValueTaggerValues(
                *patch,
                getDataContext());
        }
        
        d_value_tagger->getValueStatistics(
            patch_hierarchy,
            level_number,
            getDataContext());
    }
}


/*
 * Tag cells for refinement using value detector.
 */
void
NavierStokes::tagValueDetectorCells(
    hier::Patch& patch,
    const double regrid_time,
    const bool initial_error,
    const int tag_indx,
    const bool uses_gradient_detector_too,
    const bool uses_multiresolution_detector_too,
    const bool uses_integral_detector_too,
    const bool uses_richardson_extrapolation_too)
{
    NULL_USE(regrid_time);
    NULL_USE(initial_error);
    
    t_tagvalue->start();
    
    // Get the tags.
    boost::shared_ptr<pdat::CellData<int> > tags(
        BOOST_CAST<pdat::CellData<int>, hier::PatchData>(
            patch.getPatchData(tag_indx)));
    
#ifdef DEBUG_CHECK_ASSERTIONS
    TBOX_ASSERT(tags);
    TBOX_ASSERT(tags->getGhostCellWidth() == 0);
#endif
    
    // Initialize values of all tags to zero.
    if ((!uses_richardson_extrapolation_too) &&
        (!uses_integral_detector_too) &&
        (!uses_multiresolution_detector_too) &&
        (!uses_gradient_detector_too)) 
    {
        tags->fillAll(0);
    }
    
    // Tag the cells by using d_value_tagger.
    if (d_value_tagger != nullptr)
    {
        d_value_tagger->tagCells(
            patch,
            tags,
            getDataContext());
    }
    
    t_tagvalue->stop();
}


/*
 * Tag cells for refinement using gradient detector.
 */
void
NavierStokes::tagGradientDetectorCells(
    hier::Patch& patch,
    const double regrid_time,
    const bool initial_error,
    const int tag_indx,
    const bool uses_value_detector_too,
    const bool uses_multiresolution_detector_too,
    const bool uses_integral_detector_too,
    const bool uses_richardson_extrapolation_too)
{
    NULL_USE(regrid_time);
    NULL_USE(initial_error);
    NULL_USE(uses_value_detector_too);
    
    t_taggradient->start();
    
    // Get the tags.
    boost::shared_ptr<pdat::CellData<int> > tags(
        BOOST_CAST<pdat::CellData<int>, hier::PatchData>(
            patch.getPatchData(tag_indx)));
    
#ifdef DEBUG_CHECK_ASSERTIONS
    TBOX_ASSERT(tags);
    TBOX_ASSERT(tags->getGhostCellWidth() == 0);
#endif
    
    // Initialize values of all tags to zero.
    if ((!uses_richardson_extrapolation_too) &&
        (!uses_integral_detector_too) &&
        (!uses_multiresolution_detector_too)) 
    {
        tags->fillAll(0);
    }
    
    // Tag the cells by using d_gradient_tagger.
    if (d_gradient_tagger != nullptr)
    {
        d_gradient_tagger->tagCells(
            patch,
            tags,
            getDataContext());
    }
    
    t_taggradient->stop();
}


/*
 * Preprocess before tagging cells using multiresolution detector.
 */
void
NavierStokes::preprocessTagMultiresolutionDetectorCells(
   const boost::shared_ptr<hier::PatchHierarchy>& patch_hierarchy,
   const int level_number,
   const double regrid_time,
   const bool initial_error,
   const bool uses_value_detector_too,
   const bool uses_gradient_detector_too,
   const bool uses_integral_detector_too,
   const bool uses_richardson_extrapolation_too)
{
    NULL_USE(regrid_time);
    NULL_USE(initial_error);
    NULL_USE(uses_value_detector_too);
    NULL_USE(uses_gradient_detector_too);
    NULL_USE(uses_integral_detector_too);
    NULL_USE(uses_richardson_extrapolation_too);
    
    if (d_multiresolution_tagger != nullptr)
    {
        boost::shared_ptr<hier::PatchLevel> level(
            patch_hierarchy->getPatchLevel(level_number));
        
        for (hier::PatchLevel::iterator ip(level->begin());
             ip != level->end();
             ip++)
        {
            const boost::shared_ptr<hier::Patch>& patch = *ip;
            
            d_multiresolution_tagger->computeMultiresolutionSensorValues(
                *patch,
                getDataContext());
        }
        
        d_multiresolution_tagger->getSensorValueStatistics(
            patch_hierarchy,
            level_number,
            getDataContext());
    }
}


/*
 * Tag cells for refinement using multiresolution detector.
 */
void
NavierStokes::tagMultiresolutionDetectorCells(
    hier::Patch& patch,
    const double regrid_time,
    const bool initial_error,
    const int tag_indx,
    const bool uses_value_detector_too,
    const bool uses_gradient_detector_too,
    const bool uses_integral_detector_too,
    const bool uses_richardson_extrapolation_too)
{
    NULL_USE(regrid_time);
    NULL_USE(initial_error);
    NULL_USE(uses_value_detector_too);
    NULL_USE(uses_gradient_detector_too);
    
    t_tagmultiresolution->start();
    
    // Get the tags.
    boost::shared_ptr<pdat::CellData<int> > tags(
        BOOST_CAST<pdat::CellData<int>, hier::PatchData>(
            patch.getPatchData(tag_indx)));
    
#ifdef DEBUG_CHECK_ASSERTIONS
    TBOX_ASSERT(tags);
    TBOX_ASSERT(tags->getGhostCellWidth() == 0);
#endif
    
    // Initialize values of all tags to zero.
    if ((!uses_richardson_extrapolation_too) &&
        (!uses_integral_detector_too))
    {
        tags->fillAll(0);
    }
    
    // Tag the cells by using d_multiresolution_tagger.
    if (d_multiresolution_tagger != nullptr)
    {
        d_multiresolution_tagger->tagCells(
            patch,
            tags,
            getDataContext());
    }
    
    t_tagmultiresolution->stop();
}


void
NavierStokes::setPhysicalBoundaryConditions(
    hier::Patch& patch,
    const double fill_time,
    const hier::IntVector& ghost_width_to_fill)
{
    t_setphysbcs->start();
    
    d_Navier_Stokes_boundary_conditions->setPhysicalBoundaryConditions(
        patch,
        fill_time,
        ghost_width_to_fill,
        getDataContext());
    
    t_setphysbcs->stop();
}


void
NavierStokes::putToRestart(
    const boost::shared_ptr<tbox::Database>& restart_db) const
{
    TBOX_ASSERT(restart_db);
    
    restart_db->putString("d_project_name", d_project_name);
    
    restart_db->putIntegerArray("d_num_ghosts", &d_num_ghosts[0], d_dim.getValue());
    
    restart_db->putInteger("d_num_species", d_num_species);
    
    restart_db->putString("d_flow_model_str", d_flow_model_str);
    
    // Put the properties of d_flow_model into the restart database.
    boost::shared_ptr<tbox::Database> restart_flow_model_db =
        restart_db->putDatabase("d_flow_model_db");
    d_flow_model->putToRestart(restart_flow_model_db);
    
    restart_db->putString("d_convective_flux_reconstructor_str", d_convective_flux_reconstructor_str);
    
    // Put the properties of d_convective_flux_reconstructor into the restart database.
    boost::shared_ptr<tbox::Database> restart_convective_flux_reconstructor_db =
        restart_db->putDatabase("d_convective_flux_reconstructor_db");
    d_convective_flux_reconstructor->putToRestart(restart_convective_flux_reconstructor_db);
    
    restart_db->putString("d_diffusive_flux_reconstructor_str", d_diffusive_flux_reconstructor_str);
    
    // Put the properties of d_diffusive_flux_reconstructor into the restart database.
    boost::shared_ptr<tbox::Database> restart_diffusive_flux_reconstructor_db =
        restart_db->putDatabase("d_diffusive_flux_reconstructor_db");
    d_diffusive_flux_reconstructor->putToRestart(restart_diffusive_flux_reconstructor_db);
    
    boost::shared_ptr<tbox::Database> restart_Navier_Stokes_boundary_conditions_db =
        restart_db->putDatabase("d_Navier_Stokes_boundary_conditions_db");
    
    d_Navier_Stokes_boundary_conditions->putToRestart(restart_Navier_Stokes_boundary_conditions_db);
    
    if (d_value_tagger != nullptr)
    {
        boost::shared_ptr<tbox::Database> restart_value_tagger_db =
            restart_db->putDatabase("d_value_tagger_db");
        
        d_value_tagger->putToRestart(restart_value_tagger_db);
    }
    
    if (d_gradient_tagger != nullptr)
    {
        boost::shared_ptr<tbox::Database> restart_gradient_tagger_db =
            restart_db->putDatabase("d_gradient_tagger_db");
        
        d_gradient_tagger->putToRestart(restart_gradient_tagger_db);
    }
    
    if (d_multiresolution_tagger != nullptr)
    {
        boost::shared_ptr<tbox::Database> restart_multiresolution_tagger_db =
            restart_db->putDatabase("d_multiresolution_tagger_db");
        
        d_multiresolution_tagger->putToRestart(restart_multiresolution_tagger_db);
    }
}


#ifdef HAVE_HDF5
void
NavierStokes::registerVisItDataWriter(
    const boost::shared_ptr<appu::VisItDataWriter>& viz_writer)
{
    TBOX_ASSERT(viz_writer);
    d_visit_writer = viz_writer;
}
#endif


bool
NavierStokes::packDerivedDataIntoDoubleBuffer(
    double* buffer,
    const hier::Patch& patch,
    const hier::Box& region,
    const std::string& variable_name,
    int depth_id,
    double simulation_time) const
{
    bool data_on_patch = d_flow_model->
        packDerivedDataIntoDoubleBuffer(
            buffer,
            patch,
            region,
            variable_name,
            depth_id,
            simulation_time);
    
    return data_on_patch;
}


void NavierStokes::printClassData(std::ostream& os) const
{
    os << "\nPrint NavierStokes object... " << std::endl;
    os << std::endl;
    
    os << "NavierStokes: this = " << (NavierStokes *)this << std::endl;
    os << "d_object_name = " << d_object_name << std::endl;
    os << "d_project_name = " << d_project_name << std::endl;
    os << "d_dim = " << d_dim.getValue() << std::endl;
    os << "d_grid_geometry = " << d_grid_geometry.get() << std::endl;
    os << "d_num_ghosts = " << d_num_ghosts << std::endl;
    
    // Print all characteristics of d_flow_model.
    d_flow_model_manager->printClassData(os);
    
    os << "d_num_species = " << d_num_species << std::endl;
    
    os << "--------------------------------------------------------------------------------";
    
    /*
     * Print data of d_grid_geometry.
     */
    os << "\nPrint CartesianGridGeometry object..." << std::endl;
    os << std::endl;
    
    os << "CartesianGridGeometry: this = "
       << (geom::CartesianGridGeometry *) &d_grid_geometry
       << std::endl;
    
    os << "d_object_name = "
       << d_grid_geometry->getObjectName()
       << std::endl;
    
    const double* x_lo = d_grid_geometry->getXLower();
    const double* x_up = d_grid_geometry->getXUpper();
    
    os << "d_x_lo = (" << x_lo[0];
    for (int di = 1; di < d_dim.getValue(); di++)
    {
        os << "," << x_lo[di];
    }
    os << ")" << std::endl;
    
    os << "d_x_up = (" << x_up[0];
    for (int di = 1; di < d_dim.getValue(); di++)
    {
        os << "," << x_up[di];
    }
    os << ")" << std::endl;
    
    const double* dx = d_grid_geometry->getDx();
    
    os << "d_dx = (" << dx[0];
    for (int di = 1; di < d_dim.getValue(); di++)
    {
        os << "," << dx[di];
    }
    os << ")" << std::endl;
    
    const hier::BoxContainer domain_box = d_grid_geometry->getPhysicalDomain();
    
    os << "d_domain_box = ";
    domain_box.print(os);
    
    os << "d_periodic_shift = " << d_grid_geometry->getPeriodicShift(hier::IntVector::getOne(d_dim));
    os << std::endl;
    
    os << "--------------------------------------------------------------------------------";
    
    /*
     * Print data of d_convective_flux_reconstructor.
     */
    
    d_convective_flux_reconstructor->printClassData(os);
    os << "--------------------------------------------------------------------------------";
    
    /*
     * Print data of d_diffusive_flux_reconstructor.
     */
    
    d_diffusive_flux_reconstructor->printClassData(os);
    os << "--------------------------------------------------------------------------------";
    
    /*
     * Print Refinement data
     */
    
    /*
     * Print data of d_Navier_Stokes_boundary_conditions.
     */
    
    d_Navier_Stokes_boundary_conditions->printClassData(os);
}


void
NavierStokes::printErrorStatistics(
    std::ostream& os,
    const boost::shared_ptr<hier::PatchHierarchy>& patch_hierarchy) const
{
    NULL_USE(os);
    NULL_USE(patch_hierarchy);
}


void
NavierStokes::printDataStatistics(
    std::ostream& os,
    const boost::shared_ptr<hier::PatchHierarchy>& patch_hierarchy) const
{
    const tbox::SAMRAI_MPI& mpi(tbox::SAMRAI_MPI::getSAMRAIWorld());
    
    math::HierarchyCellDataOpsReal<double> cell_double_operator(patch_hierarchy, 0, 0);
    
    hier::VariableDatabase* variable_db = hier::VariableDatabase::getDatabase();
    
    std::vector<std::string> variable_names = d_flow_model->getNamesOfConservativeVariables();
    
    std::vector<boost::shared_ptr<pdat::CellVariable<double> > > variables =
        d_flow_model->getConservativeVariables();
    
    for (int vi = 0; vi < static_cast<int>(variables.size()); vi++)
    {
        const int var_depth = variables[vi]->getDepth();
        const int var_id = variable_db->mapVariableAndContextToIndex(
            variables[vi],
            d_plot_context);
        
        double var_max_local = cell_double_operator.max(var_id);
        double var_min_local = cell_double_operator.min(var_id);
        
        double var_max_global = 0.0;
        double var_min_global = 0.0;
        
        mpi.Allreduce(
            &var_max_local,
            &var_max_global,
            1,
            MPI_DOUBLE,
            MPI_MAX);
        
        mpi.Allreduce(
            &var_min_local,
            &var_min_global,
            1,
            MPI_DOUBLE,
            MPI_MAX);
        
        if (var_depth > 1)
        {
            os << "Max/min " << variable_names[vi] << " component: " << var_max_global << "/" << var_min_global << std::endl;
        }
        else
        {
            os << "Max/min " << variable_names[vi] << ": " << var_max_global << "/" << var_min_global << std::endl;
        }
    }
}


void
NavierStokes::getFromInput(
    const boost::shared_ptr<tbox::Database>& input_db,
    bool is_from_restart)
{
    /*
     * Note: if we are restarting, then we only allow nonuniform
     * workload to be used if nonuniform workload was used originally.
     */
    if (!is_from_restart)
    {
        d_use_nonuniform_workload = input_db->
            getBoolWithDefault(
                "use_nonuniform_workload",
                d_use_nonuniform_workload);
    }
    else
    {
        if (d_use_nonuniform_workload)
        {
            d_use_nonuniform_workload = input_db->getBool("use_nonuniform_workload");
        }
    }
    
    if (!is_from_restart)
    {
        if (input_db->keyExists("project_name"))
        {
            d_project_name = input_db->getString("project_name");
        }
        else
        {
            d_project_name = "Unnamed";
        }
        
        if (input_db->keyExists("num_species"))
        {
            d_num_species = input_db->getInteger("num_species");
            
            if (d_num_species <= 0)
            {
                TBOX_ERROR(d_object_name
                    << ": "
                    << "Non-positive number of species is specified."
                    << " Number of species should be positive."
                    << std::endl);
            }
        }
        else
        {
            TBOX_ERROR(d_object_name
                << ": "
                << "Key data 'num_species' not found in input database."
                << " Number of species is unknown."
                << std::endl);
        }
        
        /*
         * Get the flow model.
         */
        if (input_db->keyExists("flow_model"))
        {
            d_flow_model_str = input_db->getString("flow_model");
        }
        else
        {
            TBOX_ERROR(d_object_name
                << ": "
                << "Key data 'flow_model' not found in input database."
                << " Compressible flow model is unknown."
                << std::endl);            
        }
        
        /*
         * Get the database of the flow model.
         */
        if (input_db->keyExists("Flow_model"))
        {
            d_flow_model_db = input_db->getDatabase("Flow_model");
        }
        else
        {
            TBOX_ERROR(d_object_name
                << ": "
                << "Key data 'Flow_model' not found in input database."
                << std::endl);
        }
        
        /*
         * Get the convective flux reconstructor.
         */
        if (input_db->keyExists("convective_flux_reconstructor"))
        {
            d_convective_flux_reconstructor_str = input_db->getString("convective_flux_reconstructor");
        }
        else
        {
            TBOX_ERROR(d_object_name
                << ": "
                << "Key data 'convective_flux_reconstructor' not found in input database."
                << std::endl);            
        }
        
        /*
         * Get the database of the convective flux reconstructor.
         */
        if (input_db->keyExists("Convective_flux_reconstructor"))
        {
            d_convective_flux_reconstructor_db = input_db->getDatabase("Convective_flux_reconstructor");
        }
        else
        {
            TBOX_ERROR(d_object_name
                << ": "
                << "Key data 'Convective_flux_reconstructor' not found in input database."
                << std::endl);
        }
        
        /*
         * Get the diffusive flux reconstructor.
         */
        if (input_db->keyExists("diffusive_flux_reconstructor"))
        {
            d_diffusive_flux_reconstructor_str = input_db->getString("diffusive_flux_reconstructor");
        }
        else
        {
            TBOX_ERROR(d_object_name
                << ": "
                << "Key data 'diffusive_flux_reconstructor' not found in input database."
                << std::endl);            
        }
        
        /*
         * Get the database of the diffusive flux reconstructor.
         */
        if (input_db->keyExists("Diffusive_flux_reconstructor"))
        {
            d_diffusive_flux_reconstructor_db = input_db->getDatabase("Diffusive_flux_reconstructor");
        }
        else
        {
            TBOX_ERROR(d_object_name
                << ": "
                << "Key data 'Diffusive_flux_reconstructor' not found in input database."
                << std::endl);
        }
        
        if (input_db->keyExists("Value_tagger"))
        {
            d_value_tagger_db =
                input_db->getDatabase("Value_tagger");
        }
        
        if (input_db->keyExists("Gradient_tagger"))
        {
            d_gradient_tagger_db =
                input_db->getDatabase("Gradient_tagger");
        }
        
        if (input_db->keyExists("Multiresolution_tagger"))
        {
            d_multiresolution_tagger_db =
                input_db->getDatabase("Multiresolution_tagger");
        }
    }
    
    /*
     * Get the boundary conditions from the input database.
     */
    
    const hier::IntVector &one_vec = hier::IntVector::getOne(d_dim);
    hier::IntVector periodic = d_grid_geometry->getPeriodicShift(one_vec);
    int num_per_dirs = 0;
    for (int di = 0; di < d_dim.getValue(); di++)
    {
        if (periodic(di))
        {
            num_per_dirs++;
        }
    }
    
    if (num_per_dirs < d_dim.getValue())
    {
        if (input_db->keyExists("Boundary_data"))
        {
            d_Navier_Stokes_boundary_conditions_db = input_db->getDatabase(
                "Boundary_data");
            
            d_Navier_Stokes_boundary_conditions_db_is_from_restart = false;
        }
        else
        {
            if (!is_from_restart)
            {
                TBOX_ERROR(d_object_name
                    << ": "
                    << "Key data 'Boundary_data' not found in input database."
                    << std::endl);
            }
        }
    }
}


void NavierStokes::getFromRestart()
{
    boost::shared_ptr<tbox::Database> root_db(tbox::RestartManager::getManager()->getRootDatabase());
    
    if (!root_db->isDatabase(d_object_name))
    {
        TBOX_ERROR("Restart database corresponding to "
                   << d_object_name
                   << " not found in restart file."
                   << std::endl);
    }
    
    boost::shared_ptr<tbox::Database> db(root_db->getDatabase(d_object_name));
    
    d_project_name = db->getString("d_project_name");
    
    int* tmp_num_ghosts = &d_num_ghosts[0];
    db->getIntegerArray("d_num_ghosts", tmp_num_ghosts, d_dim.getValue());
    
    d_num_species = db->getInteger("d_num_species");
    
    d_flow_model_str = db->getString("d_flow_model_str");
    
    d_flow_model_db = db->getDatabase("d_flow_model_db");
    
    d_convective_flux_reconstructor_str = db->getString("d_convective_flux_reconstructor_str");
    
    d_convective_flux_reconstructor_db = db->getDatabase("d_convective_flux_reconstructor_db");
    
    d_diffusive_flux_reconstructor_str = db->getString("d_diffusive_flux_reconstructor_str");
    
    d_diffusive_flux_reconstructor_db = db->getDatabase("d_diffusive_flux_reconstructor_db");
    
    d_Navier_Stokes_boundary_conditions_db = db->getDatabase("d_Navier_Stokes_boundary_conditions_db");
    
    d_Navier_Stokes_boundary_conditions_db_is_from_restart = true;
    
    if (db->keyExists("d_value_tagger_db"))
    {
        d_value_tagger_db = db->getDatabase("d_value_tagger_db");
    }
    
    if (db->keyExists("d_gradient_tagger_db"))
    {
        d_gradient_tagger_db = db->getDatabase("d_gradient_tagger_db");
    }
    
    if (db->keyExists("d_multiresolution_tagger_db"))
    {
        d_multiresolution_tagger_db = db->getDatabase("d_multiresolution_tagger_db");
    }
}