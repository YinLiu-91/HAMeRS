#ifndef FLOW_MODEL_SOURCE_UTILITIES_FIVE_EQN_ALLAIRE_HPP
#define FLOW_MODEL_SOURCE_UTILITIES_FIVE_EQN_ALLAIRE_HPP

#include "flow/flow_models/FlowModelSourceUtilities.hpp"

class FlowModelSourceUtilitiesFiveEqnAllaire: public FlowModelSourceUtilities
{
    public:
        FlowModelSourceUtilitiesFiveEqnAllaire(
            const std::string& object_name,
            const tbox::Dimension& dim,
            const boost::shared_ptr<geom::CartesianGridGeometry>& grid_geometry,
            const int& num_species,
            const boost::shared_ptr<tbox::Database>& flow_model_db,
            const boost::shared_ptr<EquationOfStateMixingRules> equation_of_state_mixing_rules):
                FlowModelSourceUtilities(
                    object_name,
                    dim,
                    grid_geometry,
                    num_species,
                    dim.getValue() + 2*num_species,
                    flow_model_db),
            d_equation_of_state_mixing_rules(equation_of_state_mixing_rules)
        {}
        
        ~FlowModelSourceUtilitiesFiveEqnAllaire() {}
        
        /*
         * Compute the source on a patch.
         */
        void
        computeSourceOnPatch(
            hier::Patch& patch,
            const boost::shared_ptr<pdat::CellVariable<double> >& variable_source,
            const boost::shared_ptr<hier::VariableContext>& data_context,
            const double time,
            const double dt,
            const int RK_step_number);
        
    private:
        /*
         * boost::shared_ptr to EquationOfStateMixingRules.
         */
        const boost::shared_ptr<EquationOfStateMixingRules>
            d_equation_of_state_mixing_rules;
        
};

#endif /* FLOW_MODEL_SOURCE_UTILITIES_FIVE_EQN_ALLAIRE_HPP */
