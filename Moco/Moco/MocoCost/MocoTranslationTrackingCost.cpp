/* -------------------------------------------------------------------------- *
 * OpenSim Moco: MocoTranslationTrackingCost.h                                *
 * -------------------------------------------------------------------------- *
 * Copyright (c) 2019 Stanford University and the Authors                     *
 *                                                                            *
 * Author(s): Nicholas Bianco                                                 *
 *                                                                            *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may    *
 * not use this file except in compliance with the License. You may obtain a  *
 * copy of the License at http://www.apache.org/licenses/LICENSE-2.0          *
 *                                                                            *
 * Unless required by applicable law or agreed to in writing, software        *
 * distributed under the License is distributed on an "AS IS" BASIS,          *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.   *
 * See the License for the specific language governing permissions and        *
 * limitations under the License.                                             *
 * -------------------------------------------------------------------------- */

#include "MocoTranslationTrackingCost.h"

#include <algorithm>
#include "../MocoUtilities.h"
#include <OpenSim/Simulation/StatesTrajectory.h>
#include <OpenSim/Simulation/Model/Model.h>

using namespace OpenSim;
using SimTK::Vec3;

void MocoTranslationTrackingCost::initializeOnModelImpl(const Model& model)
        const {
    // Get the reference data.
    TimeSeriesTableVec3 translationTable;
    std::vector<std::string> pathsToUse;
    if (m_translation_table.getNumColumns() != 0) { // translation table provided
        // Should not be able to supply any two simultaneously.
        assert(get_reference_file() == "");
        assert(m_states_table.getNumColumns() == 0);

        // If the frame_paths property is empty, use all frame paths specified
        // in the table's column labels. Otherwise, select only the columns 
        // from the tabel that correspond with paths in frame_paths.
        if (!getProperty_frame_paths().empty()) {
            pathsToUse = m_translation_table.getColumnLabels();
            translationTable = m_translation_table;
        } else {
            translationTable = TimeSeriesTableVec3(
                m_translation_table.getIndependentColumn());
            const auto& labels = m_translation_table.getColumnLabels();
            for (int i = 0; i < getProperty_frame_paths().size(); ++i) {
                const auto& path = get_frame_paths(i);
                OPENSIM_THROW_IF_FRMOBJ(
                    std::find(labels.begin(), labels.end(), path) ==
                    labels.end(),
                    Exception,
                    format("Expected frame_paths to match at least one of the "
                        "column labels in the translation reference, but frame "
                        "path '%s' not found in the reference labels.", path));
                pathsToUse.push_back(path);
                translationTable.appendColumn(path,
                    m_translation_table.getDependentColumn(path));
            }
        }

    } else { // states reference file or states reference provided
        TimeSeriesTable tableToUse;
        if (get_reference_file() != "") { // states reference file
            // Should not be able to supply any two simultaneously.
            assert(m_states_table.getNumColumns() == 0);
            assert(m_translation_table.getNumColumns() == 0);

            auto tablesFromFile = FileAdapter::readFile(get_reference_file());
            // There should only be one table.
            OPENSIM_THROW_IF_FRMOBJ(tablesFromFile.size() != 1, Exception,
                format("Expected reference file '%s' to contain 1 table, but "
                    "it contains %i tables.", get_reference_file(), 
                    tablesFromFile.size()));
            // Get the first table.
            auto* firstTable =
                dynamic_cast<TimeSeriesTable*>(
                    tablesFromFile.begin()->second.get());
            OPENSIM_THROW_IF_FRMOBJ(!firstTable, Exception,
                "Expected reference file to contain a (scalar) "
                "TimeSeriesTable, but it contains a different type of table.");
            tableToUse = *firstTable;

        } else if (m_states_table.getNumColumns() != 0) { // states reference
            // Should not be able to supply any two simultaneously.
            assert(get_reference_file() == "");
            assert(m_translation_table.getNumColumns() == 0);
            tableToUse = m_states_table;

        } else {
            OPENSIM_THROW_FRMOBJ(Exception,
                "Expected user to either provide a reference "
                "file or to programmatically provide a reference table, but "
                "the user supplied neither.");
        }

        // Check that the reference state names match the model state names.
        auto modelStateNames = model.getStateVariableNames();
        auto tableStateNames = tableToUse.getColumnLabels();
        for (int i = 0; i < modelStateNames.getSize(); ++i) {
            const auto& name = modelStateNames[i];
            OPENSIM_THROW_IF_FRMOBJ(std::count(tableStateNames.begin(),
                    tableStateNames.end(), name) == 0,
                Exception, format("Expected the reference state names to match "
                    "the model state names, but reference state %s not found "
                    "in the model.", name));
        }

        // Create the StatesTrajectory.
        Storage sto = convertTableToStorage(tableToUse);
        auto statesTraj = StatesTrajectory::createFromStatesStorage(model, sto);

        // Use all paths provided in frame_paths.
        OPENSIM_THROW_IF_FRMOBJ(getProperty_frame_paths().empty(), Exception,
            "Expected paths in the frame_paths property, but none were found.");
        for (int i = 0; i < getProperty_frame_paths().size(); ++i) {
            pathsToUse.push_back(get_frame_paths(i));
        }

        // Use the StatesTrajectory to create the table of translation data to
        // be used in the cost.
        for (auto state : statesTraj) {
            model.getSystem().prescribe(state);
            model.realizePosition(state);
            std::vector<Vec3> translations;
            for (const auto& path : pathsToUse) {
                Vec3 translation =
                    model.getComponent<Frame>(path).getPositionInGround(state);
                translations.push_back(translation);
            }
            translationTable.appendRow(state.getTime(), translations);
        }
        translationTable.setColumnLabels(pathsToUse);

    }

    // Cache the model frames and translation weights based on the order of the 
    // translation table.
    for (int i = 0; i < pathsToUse.size(); ++i) {
        const auto& path = pathsToUse[i];
        const auto& frame = model.getComponent<Frame>(path);
        m_model_frames.emplace_back(&frame);

        double weight = 1.0;
        if (get_translation_weights().contains(path)) {
            weight = get_translation_weights().get(path).getWeight();
        }
        m_translation_weights.push_back(weight);
    }

    // Create a new scalar-valued TimeSeriesTable using the time index from the
    // translation table argument. We'll populate this table with the 
    // translation values we need when calculating the integral tracking cost, 
    // namely the frame position vector elements.
    TimeSeriesTable flatTable(translationTable.getIndependentColumn());

    // This matrix has the input table number of columns times three to hold all
    // position elements per translation.
    SimTK::Matrix mat((int)translationTable.getNumRows(), 
                    3*(int)translationTable.getNumColumns());
    // Column labels are necessary for creating the GCVSplineSet from the table,
    // so we'll label each column using the frame path and the position vector
    // element (e.g. "<frame-path>/position_p0" for the first position vector
    // element).
    std::vector<std::string> colLabels;
    for (int irow = 0; irow < translationTable.getNumRows(); ++irow) {
        const auto row = translationTable.getRowAtIndex(irow);

        // Get position vector elements.
        int icol = 0;
        for (int ielt = 0; ielt < row.size(); ++ielt) {
            const auto& label = translationTable.getColumnLabel(ielt);
            const auto& p = row[ielt];
            for (int ip = 0; ip < p.size(); ++ip) {
                mat.updElt(irow, icol++) = p[ip];
                if (!irow) {
                    colLabels.push_back(format("%s/position_p%i", label, ip));
                }
            }
        }
    }

    flatTable.updMatrix() = mat;
    flatTable.setColumnLabels(colLabels);

    m_ref_splines = GCVSplineSet(flatTable);
}

void MocoTranslationTrackingCost::calcIntegralCostImpl(const SimTK::State& state,
    double& integrand) const {
    const auto& time = state.getTime();
    getModel().realizePosition(state);
    SimTK::Vector timeVec(1, time);

    integrand = 0;
    for (int iframe = 0; iframe < (int)m_model_frames.size(); ++iframe) {
        const auto& p_model =
            m_model_frames[iframe]->getPositionInGround(state);

        // Compute position error.
        Vec3 p_ref(0.0);
        for (int ip = 0; ip < p_ref.size(); ++ip) {
            p_ref[ip] = m_ref_splines[3*iframe + ip].calcValue(timeVec);
        }
        Vec3 error = p_model - p_ref;

        // Add this frame's position error to the integrand.
        const double& weight = m_translation_weights[iframe];
        integrand += weight * error.normSqr();
    }
}