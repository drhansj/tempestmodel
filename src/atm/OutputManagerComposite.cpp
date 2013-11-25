///////////////////////////////////////////////////////////////////////////////
///
///	\file    OutputManagerComposite.cpp
///	\author  Paul Ullrich
///	\version August 11, 2010
///
///	<remarks>
///		Copyright 2000-2010 Paul Ullrich
///
///		This file is distributed as part of the Tempest source code package.
///		Permission is granted to use, copy, modify and distribute this
///		source code and its documentation under the terms of the GNU General
///		Public License.  This software is provided "as is" without express
///		or implied warranty.
///	</remarks>

#include "OutputManagerComposite.h"

#include "Model.h"
#include "Grid.h"
#include "ConsolidationStatus.h"

#include "TimeObj.h"
#include "Announce.h"

#include "mpi.h"

#include <iostream>
#include <cstdio>
#include <cmath>
#include <cfloat>
#include <sys/stat.h>

///////////////////////////////////////////////////////////////////////////////

OutputManagerComposite::OutputManagerComposite(
	Grid & grid,
	double dOutputDeltaT,
	std::string strOutputDir,
	std::string strOutputFormat,
	std::string strRestartFile
) :
	OutputManager(
		grid,
		dOutputDeltaT,
		strOutputDir,
		strOutputFormat,
		1),
	m_pActiveNcOutput(NULL),
	m_strRestartFile(strRestartFile)
{
}

///////////////////////////////////////////////////////////////////////////////

OutputManagerComposite::~OutputManagerComposite() {
	CloseFile();
}

///////////////////////////////////////////////////////////////////////////////

bool OutputManagerComposite::OpenFile(
	const std::string & strFileName
) {
	// Determine processor rank
	int nRank;
	MPI_Comm_rank(MPI_COMM_WORLD, &nRank);

	// The active model
	const Model & model = m_grid.GetModel();

	// Open new NetCDF file
	NcVar * varZs;

	if (nRank == 0) {

		// Allocate receive buffer
		m_vecRecvBuffer.Initialize(m_grid.GetMaxDegreesOfFreedom());

		// Check for existing NetCDF file
		if (m_pActiveNcOutput != NULL) {
			_EXCEPTIONT("NetCDF file already open");
		}

		// Open new NetCDF file
		std::string strNcFileName = strFileName + ".restart.nc";
		m_pActiveNcOutput = new NcFile(strNcFileName.c_str(), NcFile::Replace);
		if (m_pActiveNcOutput == NULL) {
			_EXCEPTIONT("Error opening NetCDF file");
		}

		// Create nodal index dimension
		NcDim * dimNodeIndex2D =
			m_pActiveNcOutput->add_dim(
				"node_index_2d", m_grid.GetTotalNodeCount2D());

		NcDim * dimNodeIndex =
			m_pActiveNcOutput->add_dim(
				"node_index", m_grid.GetTotalNodeCount(DataLocation_Node));

		NcDim * dimREdgeIndex =
			m_pActiveNcOutput->add_dim(
				"redge_index", m_grid.GetTotalNodeCount(DataLocation_REdge));

		// Output start time and current time
		m_pActiveNcOutput->add_att("start_time",
			model.GetStartTime().ToShortString().c_str());

		// Output physical constants
		const PhysicalConstants & phys = model.GetPhysicalConstants();

		m_pActiveNcOutput->add_att("earth_radius", phys.GetEarthRadius());
		m_pActiveNcOutput->add_att("g", phys.GetG());
		m_pActiveNcOutput->add_att("omega", phys.GetOmega());
		m_pActiveNcOutput->add_att("alpha", phys.GetAlpha());
		m_pActiveNcOutput->add_att("Rd", phys.GetR());
		m_pActiveNcOutput->add_att("Cp", phys.GetCp());
		m_pActiveNcOutput->add_att("T0", phys.GetT0());
		m_pActiveNcOutput->add_att("P0", phys.GetP0());
		m_pActiveNcOutput->add_att("rho_water", phys.GetRhoWater());
		m_pActiveNcOutput->add_att("Rvap", phys.GetRvap());
		m_pActiveNcOutput->add_att("Mvap", phys.GetMvap());
		m_pActiveNcOutput->add_att("Lvap", phys.GetLvap());

		// Output equation set
		const EquationSet & eqn = model.GetEquationSet();

		m_pActiveNcOutput->add_att("equation_set", eqn.GetName().c_str());

		// Output the grid
		m_grid.ToFile(*m_pActiveNcOutput);

		// Get the patch index dimension
		NcDim * dimPatchIndex = m_pActiveNcOutput->get_dim("patch_index");
		if (dimPatchIndex == NULL) {
			_EXCEPTIONT("Dimension \'patch_index\' not found");
		}

		// Create variables
		for (int c = 0; c < eqn.GetComponents(); c++) {
			if (m_grid.GetVarLocation(c) == DataLocation_Node) {
				m_vecComponentVar.push_back(
					m_pActiveNcOutput->add_var(
						eqn.GetComponentShortName(c).c_str(),
						ncDouble, dimNodeIndex));

			} else if (m_grid.GetVarLocation(c) == DataLocation_REdge) {
				m_vecComponentVar.push_back(
					m_pActiveNcOutput->add_var(
						eqn.GetComponentShortName(c).c_str(),
						ncDouble, dimREdgeIndex));

			} else {
				_EXCEPTIONT("(UNIMPLEMENTED) Invalid DataLocation");
			}
		}

		for (int c = 0; c < eqn.GetTracers(); c++) {
			m_vecTracersVar.push_back(
				m_pActiveNcOutput->add_var(
					eqn.GetTracerShortName(c).c_str(),
					ncDouble, dimNodeIndex));
		}

		// Output topography for each patch
		varZs = m_pActiveNcOutput->add_var("ZS", ncDouble, dimNodeIndex2D);
	}

	// Begin data consolidation
	std::vector<DataTypeLocationPair> vecDataTypes;
	vecDataTypes.push_back(DataType_Topography);

	ConsolidationStatus status(m_grid, vecDataTypes);

	m_grid.ConsolidateDataToRoot(status);

#pragma message "Move to Output"
	while ((nRank == 0) && (!status.Done())) {

		int nRecvCount;
		int ixRecvPatch;
		DataType eRecvDataType;
		DataLocation eRecvDataLocation;

		m_grid.ConsolidateDataAtRoot(
			status,
			m_vecRecvBuffer,
			nRecvCount,
			ixRecvPatch,
			eRecvDataType,
			eRecvDataLocation);

		// Store topography data
		if (eRecvDataType == DataType_Topography) {
			int nPatchIx = m_grid.GetCumulativePatch2DNodeIndex(ixRecvPatch);
			varZs->set_cur(nPatchIx);
			varZs->put(&(m_vecRecvBuffer[0]), nRecvCount);

		} else {
			_EXCEPTIONT("Invalid DataType");
		}
	}

	// Wait for all processes to complete
	MPI_Barrier(MPI_COMM_WORLD);

	return true;
}	

///////////////////////////////////////////////////////////////////////////////

void OutputManagerComposite::CloseFile() {
	if (m_pActiveNcOutput != NULL) {
		delete(m_pActiveNcOutput);
		m_pActiveNcOutput = NULL;

		m_vecComponentVar.clear();
		m_vecTracersVar.clear();

		m_vecRecvBuffer.Deinitialize();
	}
}

///////////////////////////////////////////////////////////////////////////////

void OutputManagerComposite::Output(
	const Time & time
) {
	// Check for open file
	if (!IsFileOpen()) {
		_EXCEPTIONT("No file available for output");
	}

	// Verify that only one output has been performed
	if (m_ixOutputTime != 0) {
		_EXCEPTIONT("Only one Composite output allowed per file");
	}

	// Determine processor rank
	int nRank;
	MPI_Comm_rank(MPI_COMM_WORLD, &nRank);

	// Equation set
	const EquationSet & eqn = m_grid.GetModel().GetEquationSet();

	// Output start time and current time
	if (nRank == 0) {
		m_pActiveNcOutput->add_att(
			"current_time", time.ToShortString().c_str());
	}

	// Begin data consolidation
	std::vector<DataTypeLocationPair> vecDataTypes;
	if (m_grid.GetVarsAtLocation(DataLocation_Node) != 0) {
		vecDataTypes.push_back(
			DataTypeLocationPair(DataType_State, DataLocation_Node));
	}
	if (m_grid.GetVarsAtLocation(DataLocation_REdge) != 0) {
		vecDataTypes.push_back(
			DataTypeLocationPair(DataType_State, DataLocation_REdge));
	}
	if (eqn.GetTracers() != 0) {
		vecDataTypes.push_back(DataType_Tracers);
	}

	ConsolidationStatus status(m_grid, vecDataTypes);

	m_grid.ConsolidateDataToRoot(status);

	// Receive all data objects from neighbors
	while ((nRank == 0) && (!status.Done())) {
		int nRecvCount;
		int ixRecvPatch;
		DataType eRecvDataType;
		DataLocation eRecvDataLocation;

		m_grid.ConsolidateDataAtRoot(
			status,
			m_vecRecvBuffer,
			nRecvCount,
			ixRecvPatch,
			eRecvDataType,
			eRecvDataLocation);

		// Store state variable data
		if (eRecvDataType == DataType_State) {
			if (eRecvDataLocation == DataLocation_Node) {
				if (nRecvCount % m_grid.GetRElements() != 0) {
					_EXCEPTIONT("Invalid message length");
				}

			} else if (eRecvDataLocation == DataLocation_REdge) {
				if (nRecvCount % (m_grid.GetRElements()+1) != 0) {
					_EXCEPTIONT("Invalid message length");
				}

			} else {
				_EXCEPTIONT("Invalid DataLocation");
			}

			int ixRecvPtr = 0;

			int ixCumulative2DNode =
				m_grid.GetCumulativePatch2DNodeIndex(ixRecvPatch);

			for (int c = 0; c < eqn.GetComponents(); c++) {

				// Advance the pointer in the receive buffer
				int nComponentSize =
					m_grid.GetPatch(ixRecvPatch)->GetTotalNodeCount(
						eRecvDataLocation);

				// Only output data at relevant location
				if (m_grid.GetVarLocation(c) == eRecvDataLocation) {
					int nRadialDegreesOfFreedom;
					if (eRecvDataLocation == DataLocation_Node) {
						nRadialDegreesOfFreedom = m_grid.GetRElements();
					} else if (eRecvDataLocation == DataLocation_REdge) {
						nRadialDegreesOfFreedom = m_grid.GetRElements()+1;
					} else {
						_EXCEPTION();
					}

					m_vecComponentVar[c]->set_cur(
						ixCumulative2DNode * nRadialDegreesOfFreedom);
					m_vecComponentVar[c]->put(
						&(m_vecRecvBuffer[ixRecvPtr]), nComponentSize);
				}

				ixRecvPtr += nComponentSize;
			}

		// Store tracer variable data
		} else if (eRecvDataType == DataType_Tracers) {
			int nComponentSize =
				nRecvCount / eqn.GetTracers();

			if (nRecvCount % eqn.GetTracers() != 0) {
				_EXCEPTIONT("Invalid message length");
			}

			int nCumulative3DNodeIx =
				m_grid.GetCumulativePatch3DNodeIndex(ixRecvPatch);

			for (int c = 0; c < eqn.GetTracers(); c++) {
				m_vecTracersVar[c]->set_cur(nCumulative3DNodeIx);
				m_vecTracersVar[c]->put(
					&(m_vecRecvBuffer[nComponentSize * c]), nComponentSize);
			}
		}
	}

	// Barrier
	MPI_Barrier(MPI_COMM_WORLD);
}

///////////////////////////////////////////////////////////////////////////////

Time OutputManagerComposite::Input(
	const std::string & strFileName
) {
	// Set the flag indicating that output came from a restart file
	m_fFromRestartFile = true;

	// Determine processor rank
	int nRank;
	MPI_Comm_rank(MPI_COMM_WORLD, &nRank);

	// The active model
	const Model & model = m_grid.GetModel();

	// Open new NetCDF file
	NcFile * pNcFile = NULL;

	// Open NetCDF file
	pNcFile = new NcFile(strFileName.c_str(), NcFile::ReadOnly);
	if (pNcFile == NULL) {
		_EXCEPTIONT("Error opening NetCDF file");
	}

	// Get the time
	NcAtt * attCurrentTime = pNcFile->get_att("current_time");
	if (attCurrentTime == NULL) {
		_EXCEPTIONT("Attribute \'current_time\' not found in restart file");
	}
	std::string strCurrentTime = attCurrentTime->as_string(0);

	Time timeCurrent(strCurrentTime);

	// Equation set
	const EquationSet & eqn = m_grid.GetModel().GetEquationSet();

	// Input physical constants
#pragma message "Input physical constants here"

	// Input topography here
#pragma message "Input topography here"

	// Input state
	for (int n = 0; n < m_grid.GetActivePatchCount(); n++) {
		GridPatch * pPatch = m_grid.GetActivePatch(n);

#pragma "Allow for specification of data index here"
		GridData4D & data = pPatch->GetDataState(0);

		int nCumulative3DNodeIx =
			m_grid.GetCumulativePatch3DNodeIndex(pPatch->GetPatchIndex());

		int nComponentSize =
			pPatch->GetPatchBox().GetTotalNodes();

		for (int c = 0; c < eqn.GetComponents(); c++) {
			std::string strComponentName = eqn.GetComponentShortName(c);

			NcVar * var = pNcFile->get_var(strComponentName.c_str());
			if (var == NULL) {
				_EXCEPTION1("Cannot find variable \'%s\' in file",
					strComponentName.c_str());
			}
			var->set_cur(nCumulative3DNodeIx);
			var->get(data[c][0][0], nComponentSize);
		}
	}

	// Close the file
	if (pNcFile != NULL) {
		delete pNcFile;
	}

	return timeCurrent;
}

///////////////////////////////////////////////////////////////////////////////

