///////////////////////////////////////////////////////////////////////////////
///
///	\file    HighSpeedDynamics.cpp
///	\author  Paul Ullrich
///	\version February 18, 2017
///
///	<remarks>
///		Copyright 2000-2017 Paul Ullrich
///
///		This file is distributed as part of the Tempest source code package.
///		Permission is granted to use, copy, modify and distribute this
///		source code and its documentation under the terms of the GNU General
///		Public License.  This software is provided "as is" without express
///		or implied warranty.
///	</remarks>

#include "Defines.h"
#include "HighSpeedDynamics.h"
#include "PhysicalConstants.h"
#include "Model.h"
#include "Grid.h"
#include "GridGLL.h"
#include "GridPatchGLL.h"

#include "Announce.h"
#include "LinearAlgebra.h"
#include "FunctionTimer.h"

//#define FIX_ELEMENT_MASS_NONHYDRO

///////////////////////////////////////////////////////////////////////////////

HighSpeedDynamics::HighSpeedDynamics(
	Model & model,
	int nHorizontalOrder,
	int nHyperviscosityOrder,
	double dNuScalar,
	double dNuDiv,
	double dNuVort,
	double dInstepNuDiv
) :
	HorizontalDynamics(model),
	m_nHorizontalOrder(nHorizontalOrder),
	m_nHyperviscosityOrder(nHyperviscosityOrder),
	m_dNuScalar(dNuScalar),
	m_dNuDiv(dNuDiv),
	m_dNuVort(dNuVort),
	m_dInstepNuDiv(dInstepNuDiv)
{
}

///////////////////////////////////////////////////////////////////////////////

void HighSpeedDynamics::Initialize() {

#if !defined(PROGNOSTIC_CONTRAVARIANT_MOMENTA)
	_EXCEPTIONT("Prognostic covariant velocities not supported");
#endif

	// Get a copy of the GLL grid
	GridGLL * pGrid = dynamic_cast<GridGLL*>(m_model.GetGrid());
	if (pGrid == NULL) {
		_EXCEPTIONT("Grid must be of type GridGLL");
	}

	// Number of vertical levels
	const int nRElements = pGrid->GetRElements();

	// Number of tracers
	const int nTracerCount = m_model.GetEquationSet().GetTracers();

	// Vertical implicit terms (A)
	m_dA.Allocate(
		m_nHorizontalOrder,
		m_nHorizontalOrder,
		nRElements+1);

	// Vertical implicit terms (B)
	m_dB.Allocate(
		m_nHorizontalOrder,
		m_nHorizontalOrder,
		nRElements+1);

	// Vertical implicit terms (C)
	m_dC.Allocate(
		m_nHorizontalOrder,
		m_nHorizontalOrder,
		nRElements+1);

	// Vertical implicit terms (D)
	m_dD.Allocate(
		m_nHorizontalOrder,
		m_nHorizontalOrder,
		nRElements+1);

	// Initialize the diagnosed 2D kinetic energy
	m_dK2.Allocate(
		m_nHorizontalOrder,
		m_nHorizontalOrder,
		nRElements);

	// Initialize the diagnosed 2D contravariant alpha velocity
	m_d2DConUa.Allocate(
		m_nHorizontalOrder,
		m_nHorizontalOrder,
		nRElements);

	// Initialize the diagnosed 2D contravariant beta velocity
	m_d2DConUb.Allocate(
		m_nHorizontalOrder,
		m_nHorizontalOrder,
		nRElements);

	// Initialize the diagnosed 2D covariant alpha velocity
	m_d2DCovUa.Allocate(
		m_nHorizontalOrder,
		m_nHorizontalOrder,
		nRElements);

	// Initialize the diagnosed 2D covariant beta velocity
	m_d2DCovUb.Allocate(
		m_nHorizontalOrder,
		m_nHorizontalOrder,
		nRElements);

	// Initialize the diagnosed vertial alpha momentum flux
	m_dSDotUaREdge.Allocate(
		m_nHorizontalOrder,
		m_nHorizontalOrder,
		nRElements+1);

	// Initialize the diagnosed vertial beta momentum flux
	m_dSDotUbREdge.Allocate(
		m_nHorizontalOrder,
		m_nHorizontalOrder,
		nRElements+1);

	// Initialize the diagnosed vertical vertical momentum flux
	m_dSDotWNode.Allocate(
		m_nHorizontalOrder,
		m_nHorizontalOrder,
		nRElements);

	// Initialize the alpha and beta mass fluxes
	m_dAlphaMassFlux.Allocate(
		m_nHorizontalOrder,
		m_nHorizontalOrder,
		nRElements);

	m_dBetaMassFlux.Allocate(
		m_nHorizontalOrder,
		m_nHorizontalOrder,
		nRElements);

	// Initialize the alpha and beta momentum fluxes
	m_dAlphaVerticalMomentumFluxREdge.Allocate(
		m_nHorizontalOrder,
		m_nHorizontalOrder,
		nRElements+1);

	m_dBetaVerticalMomentumFluxREdge.Allocate(
		m_nHorizontalOrder,
		m_nHorizontalOrder,
		nRElements+1);

	// Initialize the alpha and beta pressure fluxes
	m_dAlphaPressureFlux.Allocate(
		m_nHorizontalOrder,
		m_nHorizontalOrder,
		nRElements);

	m_dBetaPressureFlux.Allocate(
		m_nHorizontalOrder,
		m_nHorizontalOrder,
		nRElements);

	// Acoustic pressure term used in vertical update
	m_dDpDTheta.Allocate(
		m_nHorizontalOrder,
		m_nHorizontalOrder,
		nRElements);

	// Buffer state
	m_dBufferState.Allocate(
		m_nHorizontalOrder,
		m_nHorizontalOrder,
		nRElements+1);
	
	// Initialize buffers for derivatives of Jacobian
	m_dJGradientA.Allocate(
		m_nHorizontalOrder,
		m_nHorizontalOrder,
		nRElements+1);

	m_dJGradientB.Allocate(
		m_nHorizontalOrder,
		m_nHorizontalOrder,
		nRElements+1);

	// LAPACK info structure
	m_nInfo.Allocate(
		m_nHorizontalOrder,
		m_nHorizontalOrder);
}

///////////////////////////////////////////////////////////////////////////////

void HighSpeedDynamics::FilterNegativeTracers(
	int iDataUpdate
) {
#ifdef POSITIVE_DEFINITE_FILTER_TRACERS
	// Get a copy of the GLL grid
	GridGLL * pGrid = dynamic_cast<GridGLL*>(m_model.GetGrid());

	// Number of vertical elements
	const int nRElements = pGrid->GetRElements();

	// Perform local update
	for (int n = 0; n < pGrid->GetActivePatchCount(); n++) {
		GridPatchGLL * pPatch =
			dynamic_cast<GridPatchGLL*>(pGrid->GetActivePatch(n));

		const PatchBox & box = pPatch->GetPatchBox();

		const DataArray3D<double> & dElementAreaNode =
			pPatch->GetElementAreaNode();

		DataArray4D<double> & dataUpdateTracer =
			pPatch->GetDataTracers(iDataUpdate);

		// Number of tracers
		const int nTracerCount = dataUpdateTracer.GetSize(0);

		// Get number of finite elements in each coordinate direction
		int nElementCountA = pPatch->GetElementCountA();
		int nElementCountB = pPatch->GetElementCountB();

		// Loop over all elements
		for (int a = 0; a < nElementCountA; a++) {
		for (int b = 0; b < nElementCountB; b++) {

			// Loop overall tracers and vertical levels
			for (int c = 0; c < nTracerCount; c++) {
			for (int k = 0; k < nRElements; k++) {

				// Compute total mass and non-negative mass
				double dTotalInitialMass = 0.0;
				double dTotalMass = 0.0;
				double dNonNegativeMass = 0.0;

				for (int i = 0; i < m_nHorizontalOrder; i++) {
				for (int j = 0; j < m_nHorizontalOrder; j++) {

					int iA = a * m_nHorizontalOrder + i + box.GetHaloElements();
					int iB = b * m_nHorizontalOrder + j + box.GetHaloElements();

					double dPointwiseMass =
						  dataUpdateTracer(c,iA,iB,k)
						* dElementAreaNode(iA,iB,k);

					dTotalMass += dPointwiseMass;

					if (dataUpdateTracer(c,iA,iB,k) >= 0.0) {
						dNonNegativeMass += dPointwiseMass;
					}
				}
				}
/*
				// Check for negative total mass
				if (dTotalMass < 1.0e-14) {
					printf("%i %i %i %1.15e %1.15e\n", n, a, b, dTotalInitialMass, dTotalMass);
					_EXCEPTIONT("Negative element mass detected in filter");
				}
*/
				// Apply scaling ratio to points with non-negative mass
				double dR = dTotalMass / dNonNegativeMass;

				for (int i = 0; i < m_nHorizontalOrder; i++) {
				for (int j = 0; j < m_nHorizontalOrder; j++) {

					int iA = a * m_nHorizontalOrder + i + box.GetHaloElements();
					int iB = b * m_nHorizontalOrder + j + box.GetHaloElements();

					if (dataUpdateTracer(c,iA,iB,k) > 0.0) {
						dataUpdateTracer(c,iA,iB,k) *= dR;
					} else {
						dataUpdateTracer(c,iA,iB,k) = 0.0;
					}
				}
				}
			}
			}
		}
		}
	}
#endif
}

///////////////////////////////////////////////////////////////////////////////

void HighSpeedDynamics::StepExplicit(
	int iDataInitial,
	int iDataUpdate,
	const Time & time,
	double dDeltaT
) {
	// Start the function timer
	FunctionTimer timer("CalculateTendencies");

	// Get a copy of the GLL grid
	GridGLL * pGrid = dynamic_cast<GridGLL*>(m_model.GetGrid());

	// Number of vertical elements
	const int nRElements = pGrid->GetRElements();

	// Physical constants
	const PhysicalConstants & phys = m_model.GetPhysicalConstants();

	// Indices of EquationSet variables
	const int UIx = 0;
	const int VIx = 1;
	const int PIx = 2;
	const int WIx = 3;
	const int RIx = 4;

	// Pre-calculate pressure on model levels
	pGrid->ComputePressure(iDataInitial);

	// Perform local update
	for (int n = 0; n < pGrid->GetActivePatchCount(); n++) {
		GridPatchGLL * pPatch =
			dynamic_cast<GridPatchGLL*>(pGrid->GetActivePatch(n));

		const PatchBox & box = pPatch->GetPatchBox();

		const DataArray3D<double> & dElementAreaNode =
			pPatch->GetElementAreaNode();
		const DataArray2D<double> & dJacobian2D =
			pPatch->GetJacobian2D();
		const DataArray3D<double> & dJacobian =
			pPatch->GetJacobian();
		const DataArray3D<double> & dJacobianREdge =
			pPatch->GetJacobianREdge();
		const DataArray3D<double> & dCovMetric2DA =
			pPatch->GetCovMetric2DA();
		const DataArray3D<double> & dCovMetric2DB =
			pPatch->GetCovMetric2DB();
		const DataArray3D<double> & dContraMetric2DA =
			pPatch->GetContraMetric2DA();
		const DataArray3D<double> & dContraMetric2DB =
			pPatch->GetContraMetric2DB();

		const DataArray4D<double> & dDerivRNode =
			pPatch->GetDerivRNode();
		const DataArray4D<double> & dDerivRREdge =
			pPatch->GetDerivRREdge();

		const DataArray3D<double> & dataZn =
			pPatch->GetZLevels();
		const DataArray3D<double> & dataZi =
			pPatch->GetZInterfaces();

		const DataArray2D<double> & dCoriolisF =
			pPatch->GetCoriolisF();

		// Data
		DataArray4D<double> & dataInitialNode =
			pPatch->GetDataState(iDataInitial, DataLocation_Node);

		DataArray4D<double> & dataUpdateNode =
			pPatch->GetDataState(iDataUpdate, DataLocation_Node);

		DataArray4D<double> & dataInitialREdge =
			pPatch->GetDataState(iDataInitial, DataLocation_REdge);

		DataArray4D<double> & dataUpdateREdge =
			pPatch->GetDataState(iDataUpdate, DataLocation_REdge);

		const DataArray3D<double> & dataPressure =
			pPatch->GetDataPressure();

		// Element grid spacing and derivative coefficients
		const double dElementDeltaA = pPatch->GetElementDeltaA();
		const double dElementDeltaB = pPatch->GetElementDeltaB();

		const double dInvElementDeltaA = 1.0 / dElementDeltaA;
		const double dInvElementDeltaB = 1.0 / dElementDeltaB;

		const DataArray2D<double> & dDxBasis1D = pGrid->GetDxBasis1D();
		const DataArray2D<double> & dStiffness1D = pGrid->GetStiffness1D();

		// Get number of finite elements in each coordinate direction
		const int nElementCountA = pPatch->GetElementCountA();
		const int nElementCountB = pPatch->GetElementCountB();

		// Loop over all elements
		for (int a = 0; a < nElementCountA; a++) {
		for (int b = 0; b < nElementCountB; b++) {

			const int iElementA = a * m_nHorizontalOrder + box.GetHaloElements();
			const int iElementB = b * m_nHorizontalOrder + box.GetHaloElements();

			// Perform interpolation from levels to interfaces and calculate
			// auxiliary quantities on model interfaces.
			for (int i = 0; i < m_nHorizontalOrder; i++) {
			for (int j = 0; j < m_nHorizontalOrder; j++) {
#pragma simd
			for (int k = 1; k < nRElements; k++) {

				const int iA = iElementA + i;
				const int iB = iElementB + j;

				dataInitialREdge(RIx,iA,iB,k) = 0.5 * (
					  dataInitialNode(RIx,iA,iB,k-1)
					+ dataInitialNode(RIx,iA,iB,k  ));

				const double dInvRhoREdge =
					1.0 / dataInitialREdge(RIx,iA,iB,k);

				dataInitialREdge(UIx,iA,iB,k) = 0.5 * (
					  dataInitialNode(UIx,iA,iB,k-1)
					+ dataInitialNode(UIx,iA,iB,k  ));

				dataInitialREdge(VIx,iA,iB,k) = 0.5 * (
					  dataInitialNode(VIx,iA,iB,k-1)
					+ dataInitialNode(VIx,iA,iB,k  ));

				// Note that we store theta not rhotheta
				dataInitialREdge(PIx,iA,iB,k) =
					dInvRhoREdge * 0.5 *
						(dataInitialNode(PIx,iA,iB,k-1)
						+ dataInitialNode(PIx,iA,iB,k));

				// Vertical flux of conserved quantities
				const double dSDotREdge =
					dataInitialREdge(WIx,iA,iB,k)
						- dataInitialREdge(UIx,iA,iB,k)
							* dDerivRREdge(iA,iB,k,0)
						- dataInitialREdge(VIx,iA,iB,k)
							* dDerivRREdge(iA,iB,k,1);

				const double dSDotInvRhoREdge =
					dSDotREdge * dInvRhoREdge;

				m_dSDotUaREdge(i,j,k) =
					dSDotInvRhoREdge
					* dataInitialREdge(UIx,iA,iB,k);

				m_dSDotUbREdge(i,j,k) =
					dSDotInvRhoREdge
					* dataInitialREdge(VIx,iA,iB,k);

				// Horizontal vertical momentum flux
				const double dVerticalMomentumBaseFluxREdge =
					dJacobianREdge(iA,iB,k)
					* dataInitialREdge(WIx,iA,iB,k)
					* dInvRhoREdge;

				m_dAlphaVerticalMomentumFluxREdge(i,j,k) =
					dVerticalMomentumBaseFluxREdge
					* dataInitialREdge(UIx,iA,iB,k);

				m_dBetaVerticalMomentumFluxREdge(i,j,k) =
					dVerticalMomentumBaseFluxREdge
					* dataInitialREdge(VIx,iA,iB,k);
			}
			}
			}

			// Calculate auxiliary quantities on model levels
			for (int i = 0; i < m_nHorizontalOrder; i++) {
			for (int j = 0; j < m_nHorizontalOrder; j++) {
#pragma simd
			for (int k = 0; k < nRElements; k++) {

				const int iA = iElementA + i;
				const int iB = iElementB + j;

				const double dRhoUa = dataInitialNode(UIx,iA,iB,k);
				const double dRhoUb = dataInitialNode(VIx,iA,iB,k);
				const double dRhoWREdge = dataInitialREdge(WIx,iA,iB,k);

				const double dInvRho = 1.0 / dataInitialNode(RIx,iA,iB,k);

				// 2D contravariant and covariant velocities
				m_d2DConUa(i,j,k) = dInvRho * dRhoUa;
				m_d2DConUb(i,j,k) = dInvRho * dRhoUb;

				m_d2DCovUa(i,j,k) =
					  dCovMetric2DA(iA,iB,0)
						* m_d2DConUa(i,j,k)
					+ dCovMetric2DA(iA,iB,1)
						* m_d2DConUb(i,j,k);

				m_d2DCovUb(i,j,k) =
					  dCovMetric2DB(iA,iB,0)
						* m_d2DConUa(i,j,k)
					+ dCovMetric2DB(iA,iB,1)
						* m_d2DConUb(i,j,k);

				// Horizontal mass fluxes
				m_dAlphaMassFlux(i,j,k) =
					dJacobian(iA,iB,k) * dRhoUa;

				m_dBetaMassFlux(i,j,k) =
					dJacobian(iA,iB,k) * dRhoUb;

				// Horizontal pressure fluxes
				m_dAlphaPressureFlux(i,j,k) =
					m_dAlphaMassFlux(i,j,k)
					* dataInitialNode(PIx,iA,iB,k)
					* dInvRho;

				m_dBetaPressureFlux(i,j,k) =
					m_dBetaMassFlux(i,j,k)
					* dataInitialNode(PIx,iA,iB,k)
					* dInvRho;

				// 2D Kinetic energy
				m_dK2(i,j,k) = 0.5 * (
					  m_d2DCovUa(i,j,k)
						* m_d2DConUa(i,j,k)
					+ m_d2DCovUb(i,j,k)
						* m_d2DConUb(i,j,k));

				// Vertical flux of momentum
				m_dSDotWNode(i,j,k) =
					0.5 * (dataInitialREdge(WIx,iA,iB,k)
						+ dataInitialREdge(WIx,iA,iB,k+1))
					- dDerivRNode(iA,iB,k,0)
						* dataInitialNode(UIx,iA,iB,k)
					- dDerivRNode(iA,iB,k,1)
						* dataInitialNode(VIx,iA,iB,k);
			}
			}
			}

			// Calculate nodal updates
			for (int i = 0; i < m_nHorizontalOrder; i++) {
			for (int j = 0; j < m_nHorizontalOrder; j++) {
#pragma simd
			for (int k = 0; k < nRElements; k++) {

				const int iA = iElementA + i;
				const int iB = iElementB + j;

				// Horizontal pressure derivatives
				double dDaP = 0.0;
				double dDbP = 0.0;

				// Derivatives of mass flux
				double dDaMassFluxAlpha = 0.0;
				double dDbMassFluxBeta = 0.0;

				// Derivatives of rhotheta flux
				double dDaPressureFluxAlpha = 0.0;
				double dDbPressureFluxBeta = 0.0;

				// Derivative of the specific kinetic energy
				double dDaKE = 0.0;
				double dDbKE = 0.0;

				// Derivatives of the 2D covariant velocity
				double dDaCovUb = 0.0;
				double dDbCovUa = 0.0;

				// Alpha derivatives
				for (int s = 0; s < m_nHorizontalOrder; s++) {

					// Alpha derivative of mass flux
					dDaMassFluxAlpha -=
						m_dAlphaMassFlux(s,j,k)
						* dStiffness1D(i,s);

					// Alpha derivative of mass flux
					dDaPressureFluxAlpha -=
						m_dAlphaPressureFlux(s,j,k)
						* dStiffness1D(i,s);
/*
					// Alpha derivative of mass flux
					dDaMassFluxAlpha +=
						m_dAlphaMassFlux(s,j,k)
						* dDxBasis1D(s,i);

					// Alpha derivative of mass flux
					dDaPressureFluxAlpha +=
						m_dAlphaPressureFlux(s,j,k)
						* dDxBasis1D(s,i);
*/
					// Alpha derivative of pressure
					dDaP +=
						dataPressure(iElementA+s,iB,k)
						* dDxBasis1D(s,i);

					// Alpha derivative of specific kinetic energy
					dDaKE +=
						m_dK2(s,j,k)
						* dDxBasis1D(s,i);

					// Alpha derivative of 2D covariant velocity
					dDaCovUb +=
						m_d2DCovUb(s,j,k)
						* dDxBasis1D(s,i);
				}

				// Beta derivatives
				for (int s = 0; s < m_nHorizontalOrder; s++) {

					// Beta derivative of mass flux
					dDbMassFluxBeta -=
						m_dBetaMassFlux(i,s,k)
						* dStiffness1D(j,s);

					// Beta derivative of mass flux
					dDbPressureFluxBeta -=
						m_dBetaPressureFlux(i,s,k)
						* dStiffness1D(j,s);
/*
					// Beta derivative of mass flux
					dDbMassFluxBeta +=
						m_dBetaMassFlux(i,s,k)
						* dDxBasis1D(s,j);

					// Beta derivative of mass flux
					dDbPressureFluxBeta +=
						m_dBetaPressureFlux(i,s,k)
						* dDxBasis1D(s,j);
*/
					// Beta derivative of pressure
					dDbP +=
						dataPressure(iA,iElementB+s,k)
						* dDxBasis1D(s,j);

					// Beta derivative of specific kinetic energy
					dDbKE +=
						m_dK2(i,s,k)
						* dDxBasis1D(s,j);

					// Beta derivative of 2D covariant velocity
					dDbCovUa +=
						m_d2DCovUa(i,s,k)
						* dDxBasis1D(s,j);
				}

				// Scale derivatives
				dDaP *= dInvElementDeltaA;
				dDbP *= dInvElementDeltaB;

				dDaMassFluxAlpha *= dInvElementDeltaA;
				dDbMassFluxBeta *= dInvElementDeltaB;

				dDaPressureFluxAlpha *= dInvElementDeltaA;
				dDbPressureFluxBeta *= dInvElementDeltaB;

				dDaKE *= dInvElementDeltaA;
				dDbKE *= dInvElementDeltaB;

				dDaCovUb *= dInvElementDeltaA;
				dDbCovUa *= dInvElementDeltaB;

				// Convert derivatives of pressure along s surface
				// to derivatives along z surfaces.
				double dDzP = 0.0;
				if (k == 0) {
					dDzP = (dataPressure(iA,iB,k+1)
							- dataPressure(iA,iB,k))
						/ (dataZn(iA,iB,k+1)
							- dataZn(iA,iB,k));

				} else if (k == nRElements-1) {
					dDzP = (dataPressure(iA,iB,k)
							- dataPressure(iA,iB,k-1))
						/ (dataZn(iA,iB,k)
							- dataZn(iA,iB,k-1));

				} else {
					dDzP = (dataPressure(iA,iB,k+1)
							- dataPressure(iA,iB,k-1))
						/ (dataZn(iA,iB,k+1)
							- dataZn(iA,iB,k-1));
				}

				dDaP -= dDerivRNode(iA,iB,k,0) * dDzP;
				dDbP -= dDerivRNode(iA,iB,k,1) * dDzP;

				// Convert pressure derivatives to contravariant
				const double dConDaP =
					  dContraMetric2DA(iA,iB,0) * dDaP
					+ dContraMetric2DA(iA,iB,1) * dDbP;

				const double dConDbP =
					  dContraMetric2DB(iA,iB,0) * dDaP
					+ dContraMetric2DB(iA,iB,1) * dDbP;

				// Convert Kinetic energy derivatives to contravariant
				const double dConDaKE =
					  dContraMetric2DA(iA,iB,0) * dDaKE
					+ dContraMetric2DA(iA,iB,1) * dDbKE;

				const double dConDbKE =
					  dContraMetric2DB(iA,iB,0) * dDaKE
					+ dContraMetric2DB(iA,iB,1) * dDbKE;

				// Compute terms in tendency equation
				const double dInvJacobian =
					1.0 / dJacobian(iA,iB,k);

				const double dInvJacobian2D =
					1.0 / dJacobian2D(iA,iB);

				const double dInvDeltaZ =
					1.0 / (dataZi(iA,iB,k+1)
						- dataZi(iA,iB,k));

				// Total horizontal flux divergence
				const double dTotalHorizFluxDiv =
					dInvJacobian * (
						  dDaMassFluxAlpha
						+ dDbMassFluxBeta);

				// Vertical fluxes
				const double dDzAlphaMomentumFluxS =
					dInvDeltaZ * (
						  m_dSDotUaREdge(i,j,k+1)
						- m_dSDotUaREdge(i,j,k  ));

				const double dDzBetaMomentumFluxS =
					dInvDeltaZ * (
						  m_dSDotUbREdge(i,j,k+1)
						- m_dSDotUbREdge(i,j,k  ));

				// Compute vorticity term
				const double dAbsVorticity = 
					dCoriolisF(iA,iB)
					+ dInvJacobian2D * (dDaCovUb - dDbCovUa);

				const double dVorticityAlpha =
					- dAbsVorticity * dInvJacobian2D * m_d2DCovUb(i,j,k);

				const double dVorticityBeta =
					  dAbsVorticity * dInvJacobian2D * m_d2DCovUa(i,j,k);

				// Compose explicit tendencies on levels
				dataUpdateNode(UIx,iA,iB,k) +=
					dDeltaT * (
						- dConDaP
						- dataInitialNode(RIx,iA,iB,k)
 							* (dConDaKE + dVorticityAlpha)
						- dTotalHorizFluxDiv * m_d2DConUa(i,j,k)
						- dDzAlphaMomentumFluxS);

/*
				printf("%1.5e %1.5e %1.5e %1.5e %1.5e %1.5e : %1.5e\n",
					- dDaP,
					- dConDaP,
					- dataInitialNode(RIx,iA,iB,k) * dConDaKE,
					- dTotalHorizFluxDiv * m_d2DConUa(i,j,k),
					- dDsAlphaMomentumFluxS,
					- dataInitialNode(RIx,iA,iB,k) * dVorticityAlpha,
					dataUpdateNode(UIx,iA,iB,k));
*/

				dataUpdateNode(VIx,iA,iB,k) +=
					dDeltaT * (
						- dConDbP
						- dataInitialNode(RIx,iA,iB,k)
 							* (dConDbKE + dVorticityBeta)
						- dTotalHorizFluxDiv * m_d2DConUb(i,j,k)
						- dDzBetaMomentumFluxS);

/*
				if (fabs(dataInitialNode(RIx,iA,iB,k) * dConDbKE) < 0.1 * fabs(dConDbP)) {
					printf("%1.5e %1.5e %1.5e %1.5e %1.5e %1.5e : %1.5e\n",
						- dConDbP,
						- dataInitialNode(RIx,iA,iB,k) * dConDbKE,
						- dTotalHorizFluxDiv * m_d2DConUb(i,j,k),
						- dDsBetaMomentumFluxS,
						- dCoriolisF(iA,iB] * dInvJacobian2D * m_d2DCovUa[i,j,k),
						- dInvJacobian2D * (dDaCovUb - dDbCovUa) * dInvJacobian2D * m_d2DCovUa(i,j,k),
						dataUpdateNode(VIx,iA,iB,k));
				}
*/

				// Density tendencies
				dataUpdateNode(RIx,iA,iB,k) +=
					- dDeltaT * dTotalHorizFluxDiv;

				// Rhotheta (pressure) tendencies
				dataUpdateNode(PIx,iA,iB,k) +=
					- dDeltaT * dInvJacobian * (
						  dDaPressureFluxAlpha
						+ dDbPressureFluxBeta);
			}
			}
			}

			// Calculate explicit tendencies on interfaces
			// the vertical flux of horizontal momentum.
			for (int i = 0; i < m_nHorizontalOrder; i++) {
			for (int j = 0; j < m_nHorizontalOrder; j++) {
#pragma simd
			for (int k = 1; k < nRElements; k++) {

				const int iA = iElementA + i;
				const int iB = iElementB + j;

				const double dInvJacobianREdge =
					1.0 / dJacobianREdge(iA,iB,k);

				// Derivatives of vertical momentum flux
				double dDaVerticalMomentumFluxAlpha = 0.0;
				double dDbVerticalMomentumFluxBeta = 0.0;

				// Alpha derivative
				for (int s = 0; s < m_nHorizontalOrder; s++) {
					dDaVerticalMomentumFluxAlpha -=
						m_dAlphaVerticalMomentumFluxREdge(s,j,k)
						* dStiffness1D(i,s);
				}

				// Beta derivative
				for (int s = 0; s < m_nHorizontalOrder; s++) {
					dDbVerticalMomentumFluxBeta -=
						m_dBetaVerticalMomentumFluxREdge(i,s,k)
						* dStiffness1D(j,s);
				}

				dDaVerticalMomentumFluxAlpha *= dInvElementDeltaA;
				dDbVerticalMomentumFluxBeta  *= dInvElementDeltaB;

				const double dInvDeltaZ =
					1.0 / (dataZn(iA,iB,k)
						- dataZn(iA,iB,k-1));

				const double dDzVerticalMomentumFluxW =
					dInvDeltaZ
					* (m_dSDotWNode(i,j,k)
						- m_dSDotWNode(i,j,k-1));
/*
				printf("%1.10e %1.10e %1.10e %1.10e\n",
					dDzPressure,
					dataInitialREdge(RIx,iA,iB,k) * phys.GetG(),
					dInvJacobianREdge * (
						  dDaVerticalMomentumFluxAlpha
						+ dDbVerticalMomentumFluxBeta),
					dDzVerticalMomentumFluxW
					);
*/
				dataUpdateREdge(WIx,iA,iB,k) +=
					- dDeltaT * (
						+ dInvJacobianREdge * (
							  dDaVerticalMomentumFluxAlpha
							+ dDbVerticalMomentumFluxBeta)
						+ dDzVerticalMomentumFluxW);
			}
			}
			}
		}
		}
	}

	// Apply direct stiffness summation to tendencies
	//pGrid->ApplyDSS(iDataUpdate, DataType_State);
}

///////////////////////////////////////////////////////////////////////////////

void HighSpeedDynamics::StepImplicit(
	int iDataInitial,
	int iDataUpdate,
	const Time & time,
	double dDeltaT
) {
	// Start the function timer
	FunctionTimer timer("AcousticLoop");

	// Get a copy of the GLL grid
	GridGLL * pGrid = dynamic_cast<GridGLL*>(m_model.GetGrid());

	// Number of vertical elements
	const int nRElements = pGrid->GetRElements();

	// Physical constants
	const PhysicalConstants & phys = m_model.GetPhysicalConstants();

	// Indices of EquationSet variables
	const int UIx = 0;
	const int VIx = 1;
	const int PIx = 2;
	const int WIx = 3;
	const int RIx = 4;

	// Update horizontal velocities
	for (int n = 0; n < pGrid->GetActivePatchCount(); n++) {
		GridPatchGLL * pPatch =
			dynamic_cast<GridPatchGLL*>(pGrid->GetActivePatch(n));

		const PatchBox & box = pPatch->GetPatchBox();

		// Metric terms
		const DataArray3D<double> & dJacobian =
			pPatch->GetJacobian();
		const DataArray3D<double> & dJacobianREdge =
			pPatch->GetJacobianREdge();

		const DataArray3D<double> & dContraMetric2DA =
			pPatch->GetContraMetric2DA();
		const DataArray3D<double> & dContraMetric2DB =
			pPatch->GetContraMetric2DB();

		const DataArray4D<double> & dDerivRNode =
			pPatch->GetDerivRNode();
		const DataArray4D<double> & dDerivRREdge =
			pPatch->GetDerivRREdge();

		const DataArray3D<double> & dataZn =
			pPatch->GetZLevels();
		const DataArray3D<double> & dataZi =
			pPatch->GetZInterfaces();

		// Data
		DataArray4D<double> & dataInitialNode =
			pPatch->GetDataState(iDataInitial, DataLocation_Node);

		DataArray4D<double> & dataInitialREdge =
			pPatch->GetDataState(iDataInitial, DataLocation_REdge);

		DataArray4D<double> & dataUpdateNode =
			pPatch->GetDataState(iDataUpdate, DataLocation_Node);

		DataArray4D<double> & dataUpdateREdge =
			pPatch->GetDataState(iDataUpdate, DataLocation_REdge);

		DataArray3D<double> & dataPressure =
			pPatch->GetDataPressure();

		// Element grid spacing and derivative coefficients
		const double dElementDeltaA = pPatch->GetElementDeltaA();
		const double dElementDeltaB = pPatch->GetElementDeltaB();

		const double dInvElementDeltaA = 1.0 / dElementDeltaA;
		const double dInvElementDeltaB = 1.0 / dElementDeltaB;

		const DataArray2D<double> & dDxBasis1D = pGrid->GetDxBasis1D();
		const DataArray2D<double> & dStiffness1D = pGrid->GetStiffness1D();

		// Get number of finite elements in each coordinate direction
		const int nElementCountA = pPatch->GetElementCountA();
		const int nElementCountB = pPatch->GetElementCountB();

		// Loop over all elements
		for (int a = 0; a < nElementCountA; a++) {
		for (int b = 0; b < nElementCountB; b++) {

			const int iElementA = a * m_nHorizontalOrder + box.GetHaloElements();
			const int iElementB = b * m_nHorizontalOrder + box.GetHaloElements();

			// Pre-compute pressure tendency
			for (int i = 0; i < m_nHorizontalOrder; i++) {
			for (int j = 0; j < m_nHorizontalOrder; j++) {
#pragma simd
			for (int k = 0; k < nRElements; k++) {

				const int iA = iElementA + i;
				const int iB = iElementB + j;

				dataPressure[iA][iB][k] =
					phys.PressureFromRhoTheta(
						dataInitialNode(PIx,iA,iB,k));

				m_dDpDTheta(i,j,k) =
					dataPressure(iA,iB,k)
					* phys.GetGamma()
					/ dataInitialNode(PIx,iA,iB,k);
			}
			}
			}

			// Compute vertical fluxes of rho and rhotheta
			for (int i = 0; i < m_nHorizontalOrder; i++) {
			for (int j = 0; j < m_nHorizontalOrder; j++) {
#pragma simd
			for (int k = 1; k < nRElements; k++) {

				const int iA = iElementA + i;
				const int iB = iElementB + j;

				dataInitialREdge(RIx,iA,iB,k) = 0.5 * (
					dataInitialNode(RIx,iA,iB,k)
					+ dataInitialNode(RIx,iA,iB,k-1));

				const double dInvRhoREdge =
					1.0 / dataInitialREdge(RIx,iA,iB,k);

				dataInitialREdge(PIx,iA,iB,k) =
					0.5 * dInvRhoREdge * (
						dataInitialNode(PIx,iA,iB,k)
						+ dataInitialNode(PIx,iA,iB,k-1));
			}
			}
			}

			// Fill in vertical column arrays
			for (int i = 0; i < m_nHorizontalOrder; i++) {
			for (int j = 0; j < m_nHorizontalOrder; j++) {
				m_dA(i,j,0) = 0.0;
				m_dB(i,j,0) = 1.0;
				m_dB(i,j,nRElements) = 1.0;
				m_dC(i,j,0) = 0.0;
				m_dD(i,j,0) = 0.0;
				m_dD(i,j,nRElements) = 0.0;
			}
			}

			// Timestep size squared
			const double dDeltaT2 = dDeltaT * dDeltaT;

			for (int i = 0; i < m_nHorizontalOrder; i++) {
			for (int j = 0; j < m_nHorizontalOrder; j++) {
#pragma simd
			for (int k = 1; k < nRElements; k++) {

				const int iA = iElementA + i;
				const int iB = iElementB + j;

				const double dInvDeltaZk =
					1.0 / (dataZi(iA,iB,k+1)
						- dataZi(iA,iB,k));

				const double dInvDeltaZkm =
					1.0 / (dataZi(iA,iB,k)
						- dataZi(iA,iB,k-1));

				const double dInvDeltaZhat =
					1.0 / (dataZn(iA,iB,k)
						- dataZn(iA,iB,k-1));

				// Note that we have stored theta in dataInitialREdge[PIx]
				m_dA(i,j,k-1) = - dDeltaT2 * dInvDeltaZkm * (
					dInvDeltaZhat
						* m_dDpDTheta(i,j,k-1)
						* dataInitialREdge(PIx,iA,iB,k-1)
					- 0.5 * phys.GetG());

				m_dB(i,j,k) = 1.0 + dDeltaT2 * (
					dInvDeltaZhat
						* dataInitialREdge(PIx,iA,iB,k)
						* (m_dDpDTheta(i,j,k) * dInvDeltaZk
							+ m_dDpDTheta(i,j,k-1) * dInvDeltaZkm)
					+ 0.5 * phys.GetG()
						* (dInvDeltaZk - dInvDeltaZkm));

				m_dC(i,j,k) = - dDeltaT2 * dInvDeltaZk * (
					+ dInvDeltaZhat
						* m_dDpDTheta(i,j,k)
						* dataInitialREdge(PIx,iA,iB,k+1)
					+ 0.5 * phys.GetG());

				const double dDzPressure = dInvDeltaZhat
					* (dataPressure(iA,iB,k)
					- dataPressure(iA,iB,k-1));

				const double dIntRho =
					phys.GetG() * dataInitialREdge(RIx,iA,iB,k);

				m_dD(i,j,k) =
					dataInitialREdge(WIx,iA,iB,k)
					- dDeltaT * (dDzPressure + dIntRho);
/*
				if ((i == 0) && (j == 0)) {
					printf("%1.5e %1.5e %1.5e %1.5e %1.5e %1.5e\n",
						dInvDeltaZhat
							* m_dDpDTheta(i,j,k-1)
							* dataInitialREdge(PIx,iA,iB,k-1),
						- 0.5 * phys.GetG(),
						+ dInvDeltaZhat
							* m_dDpDTheta(i,j,k)
							* dataInitialREdge(PIx,iA,iB,k+1),
						+ 0.5 * phys.GetG(),
						dataInitialREdge(PIx,iA,iB,k)
							* (m_dDpDTheta(i,j,k) * dInvDeltaZk
							+ m_dDpDTheta(i,j,k-1) * dInvDeltaZkm),
						0.5 * phys.GetG()
							* (dInvDeltaZk - dInvDeltaZkm));

						//dDzPressure + dIntRho, dataPressure(iA,iB,k));
						//m_dA(i,j,k-1), m_dB(i,j,k), m_dC(i,j,k), m_dD(i,j,k));
				}
*/
			}
			}
			}

			// Perform a tridiagonal solve for dataUpdate
			int nREdges = nRElements+1;
			int nRHS = 1;
			int nLDB = nREdges;

			for (int i = 0; i < m_nHorizontalOrder; i++) {
			for (int j = 0; j < m_nHorizontalOrder; j++) {

				const int iA = iElementA + i;
				const int iB = iElementB + j;

#ifdef TEMPEST_LAPACK_ACML_INTERFACE
				dgtsv(
					nREdges,
					nRHS,
					&(m_dA(i,j,0)),
					&(m_dB(i,j,0)),
					&(m_dC(i,j,0)),
					&(m_dD(i,j,0)),
					nLDB,
					&(m_nInfo(i,j)));
#endif
#ifdef TEMPEST_LAPACK_ESSL_INTERFACE
				dgtsv(
					nREdges,
					nRHS,
					&(m_dA(i,j,0)),
					&(m_dB(i,j,0)),
					&(m_dC(i,j,0)),
					&(m_dD(i,j,0)),
					nLDB);
				m_nInfo(i,j) = 0;
#endif
#ifdef TEMPEST_LAPACK_FORTRAN_INTERFACE
				dgtsv_(
					&nREdges,
					&nRHS,
					&(m_dA(i,j,0)),
					&(m_dB(i,j,0)),
					&(m_dC(i,j,0)),
					&(m_dD(i,j,0)),
					&nLDB,
					&(m_nInfo(i,j)));
#endif
/*
				if ((i == 0) && (j == 0)) {
					for (int k = 0; k <= nRElements; k++) {
						printf("0: %1.5e\n", m_dD(i,j,k));
					}
				}
*/
/*
				// DEBUG
				for (int k = 0; k < nREdges; k++) {
					m_dD(i,j,k) = dDeltaT * dataUpdateREdge(WIx,iA,iB,k);
				}
*/
			}
			}

			// Check return values from tridiagonal solve
			for (int i = 0; i < m_nHorizontalOrder; i++) {
			for (int j = 0; j < m_nHorizontalOrder; j++) {
				if (m_nInfo(i,j) != 0) {
					_EXCEPTION1("Failure in tridiagonal solve: %i",
						m_nInfo(i,j));
				}
			}
			}

			// Perform update to mass and potential temperature
			for (int i = 0; i < m_nHorizontalOrder; i++) {
			for (int j = 0; j < m_nHorizontalOrder; j++) {
#pragma simd
			for (int k = 0; k < nRElements; k++) {

				const int iA = iElementA + i;
				const int iB = iElementB + j;

				const double dInvDeltaZn =
					1.0 / (dataZi(iA,iB,k+1)
						- dataZi(iA,iB,k));

				// Store updated vertical momentum
				dataUpdateREdge(WIx,iA,iB,k) +=
					(m_dD(i,j,k)
						- dataInitialREdge(WIx,iA,iB,k));

				// Updated vertical mass flux
				const double dDzMassFlux =
					dInvDeltaZn * (
						m_dD(i,j,k+1)
						- m_dD(i,j,k));

				// Store updated density
				dataUpdateNode(RIx,iA,iB,k) +=
					- dDeltaT * dDzMassFlux;

				// Updated vertical pressure flux
				const double dDzPressureFlux = dInvDeltaZn
					* (m_dD(i,j,k+1)
						* dataInitialREdge(PIx,iA,iB,k+1)
					- m_dD(i,j,k)
						* dataInitialREdge(PIx,iA,iB,k));

				// Store updated potential temperature density
				dataUpdateNode(PIx,iA,iB,k) +=
					- dDeltaT * dDzPressureFlux;
			}
			}
			}

#pragma message "Fix"
			// Apply boundary condition to W
			for (int i = 0; i < m_nHorizontalOrder; i++) {
			for (int j = 0; j < m_nHorizontalOrder; j++) {

				const int iA = iElementA + i;
				const int iB = iElementB + j;

				dataUpdateREdge(WIx,iA,iB,0) = 0.0;
			}
			}
		}
		}
	}

	// Apply direct stiffness summation
	pGrid->ApplyDSS(iDataUpdate, DataType_State);

}

///////////////////////////////////////////////////////////////////////////////

void HighSpeedDynamics::ApplyScalarHyperdiffusion(
	int iDataInitial,
	int iDataUpdate,
	double dDeltaT,
	double dNu,
	bool fScaleNuLocally,
	int iComponent,
	bool fRemoveRefState
) {
	// Indices of EquationSet variables
	const int UIx = 0;
	const int VIx = 1;
	const int PIx = 2;
	const int WIx = 3;
	const int RIx = 4;

	// Get a copy of the GLL grid
	GridGLL * pGrid = dynamic_cast<GridGLL*>(m_model.GetGrid());

	// Number of radial elements in grid
	const int nRElements = pGrid->GetRElements();

	// Check argument
	if (iComponent < (-1)) {
		_EXCEPTIONT("Invalid component index");
	}

	// Perform local update
	for (int n = 0; n < pGrid->GetActivePatchCount(); n++) {
		GridPatchGLL * pPatch =
			dynamic_cast<GridPatchGLL*>(pGrid->GetActivePatch(n));

		const PatchBox & box = pPatch->GetPatchBox();

		const DataArray3D<double> & dElementAreaNode =
			pPatch->GetElementAreaNode();
		const DataArray3D<double> & dJacobianNode =
			pPatch->GetJacobian();
		const DataArray3D<double> & dJacobianREdge =
			pPatch->GetJacobianREdge();
		const DataArray3D<double> & dContraMetricA =
			pPatch->GetContraMetric2DA();
		const DataArray3D<double> & dContraMetricB =
			pPatch->GetContraMetric2DB();

		// Grid data
		DataArray4D<double> & dataInitialNode =
			pPatch->GetDataState(iDataInitial, DataLocation_Node);

		DataArray4D<double> & dataInitialREdge =
			pPatch->GetDataState(iDataInitial, DataLocation_REdge);

		DataArray4D<double> & dataUpdateNode =
			pPatch->GetDataState(iDataUpdate, DataLocation_Node);

		DataArray4D<double> & dataUpdateREdge =
			pPatch->GetDataState(iDataUpdate, DataLocation_REdge);

		const DataArray4D<double> & dataRefNode =
			pPatch->GetReferenceState(DataLocation_Node);

		const DataArray4D<double> & dataRefREdge =
			pPatch->GetReferenceState(DataLocation_REdge);

		// Tracer data
		DataArray4D<double> & dataInitialTracer =
			pPatch->GetDataTracers(iDataInitial);

		DataArray4D<double> & dataUpdateTracer =
			pPatch->GetDataTracers(iDataUpdate);

		// Element grid spacing and derivative coefficients
		const double dElementDeltaA = pPatch->GetElementDeltaA();
		const double dElementDeltaB = pPatch->GetElementDeltaB();

		const double dInvElementDeltaA = 1.0 / dElementDeltaA;
		const double dInvElementDeltaB = 1.0 / dElementDeltaB;

		const DataArray2D<double> & dDxBasis1D = pGrid->GetDxBasis1D();
		const DataArray2D<double> & dStiffness1D = pGrid->GetStiffness1D();

		// Number of finite elements
		int nElementCountA = pPatch->GetElementCountA();
		int nElementCountB = pPatch->GetElementCountB();

		// Compute new hyperviscosity coefficient
		double dLocalNu = dNu;

		if (fScaleNuLocally) {
			double dReferenceLength = pGrid->GetReferenceLength();
			if (dReferenceLength != 0.0) {
				dLocalNu *= pow(dElementDeltaA / dReferenceLength, 3.2);
			}
		}

		// State variables and tracers
		for (int iType = 0; iType < 2; iType++) {

			int nComponentStart;
			int nComponentEnd;

			if (iType == 0) {
				nComponentStart = 2;
				nComponentEnd = m_model.GetEquationSet().GetComponents();

				if (iComponent != (-1)) {
					if (iComponent >= nComponentEnd) {
						_EXCEPTIONT("Invalid component index");
					}

					nComponentStart = iComponent;
					nComponentEnd = iComponent+1;
				}

			} else {
				nComponentStart = 0;
				nComponentEnd = m_model.GetEquationSet().GetTracers();

				if (iComponent != (-1)) {
					continue;
				}
			}

			// Loop over all components
			for (int c = nComponentStart; c < nComponentEnd; c++) {

				int nElementCountR;

				const DataArray4D<double> * pDataInitial;
				DataArray4D<double> * pDataUpdate;
				const DataArray4D<double> * pDataRef;
				const DataArray3D<double> * pJacobian;

				if (iType == 0) {
					if (pGrid->GetVarLocation(c) == DataLocation_Node) {
						pDataInitial = &dataInitialNode;
						pDataUpdate  = &dataUpdateNode;
						pDataRef = &dataRefNode;
						nElementCountR = nRElements;
						pJacobian = &dJacobianNode;

					} else if (pGrid->GetVarLocation(c) == DataLocation_REdge) {
						pDataInitial = &dataInitialREdge;
						pDataUpdate  = &dataUpdateREdge;
						pDataRef = &dataRefREdge;
						nElementCountR = nRElements + 1;
						pJacobian = &dJacobianREdge;

					} else {
						_EXCEPTIONT("UNIMPLEMENTED");
					}

				} else {
					pDataInitial = &dataInitialTracer;
					pDataUpdate = &dataUpdateTracer;
					nElementCountR = nRElements;
					pJacobian = &dJacobianNode;
				}

				// Loop over all finite elements
				for (int a = 0; a < nElementCountA; a++) {
				for (int b = 0; b < nElementCountB; b++) {

					const int iElementA =
						a * m_nHorizontalOrder + box.GetHaloElements();
					const int iElementB =
						b * m_nHorizontalOrder + box.GetHaloElements();

					// Store the buffer state
					for (int i = 0; i < m_nHorizontalOrder; i++) {
					for (int j = 0; j < m_nHorizontalOrder; j++) {
#pragma simd
					for (int k = 0; k < nElementCountR; k++) {
						const int iA = iElementA + i;
						const int iB = iElementB + j;

						m_dBufferState(i,j,k) =
							(*pDataInitial)(c,iA,iB,k);
					}
					}
					}

					// Remove the reference state from the buffer state
					if (fRemoveRefState) {
						for (int i = 0; i < m_nHorizontalOrder; i++) {
						for (int j = 0; j < m_nHorizontalOrder; j++) {
#pragma simd
						for (int k = 0; k < nElementCountR; k++) {
							const int iA = iElementA + i;
							const int iB = iElementB + j;

							m_dBufferState(i,j,k) -=
								(*pDataRef)(c,iA,iB,k);
						}
						}
						}
					}

					// Calculate the pointwise gradient of the scalar field
					for (int i = 0; i < m_nHorizontalOrder; i++) {
					for (int j = 0; j < m_nHorizontalOrder; j++) {
#pragma simd
					for (int k = 0; k < nElementCountR; k++) {

						const int iA = iElementA + i;
						const int iB = iElementB + j;

						double dDaPsi = 0.0;
						double dDbPsi = 0.0;

						for (int s = 0; s < m_nHorizontalOrder; s++) {
							dDaPsi +=
								m_dBufferState(s,j,k)
								* dDxBasis1D(s,i);

							dDbPsi +=
								m_dBufferState(i,s,k)
								* dDxBasis1D(s,j);
						}

						dDaPsi *= dInvElementDeltaA;
						dDbPsi *= dInvElementDeltaB;

						m_dJGradientA(i,j,k) =
							(*pJacobian)(iA,iB,k) * (
								+ dContraMetricA(iA,iB,0) * dDaPsi
								+ dContraMetricA(iA,iB,1) * dDbPsi);

						m_dJGradientB(i,j,k) =
							(*pJacobian)(iA,iB,k) * (
								+ dContraMetricB(iA,iB,0) * dDaPsi
								+ dContraMetricB(iA,iB,1) * dDbPsi);
					}
					}
					}

					// Pointwise updates
					for (int i = 0; i < m_nHorizontalOrder; i++) {
					for (int j = 0; j < m_nHorizontalOrder; j++) {
#pragma simd
					for (int k = 0; k < nElementCountR; k++) {

						const int iA = iElementA + i;
						const int iB = iElementB + j;

						// Inverse Jacobian and Jacobian
						const double dInvJacobian =
							1.0 / (*pJacobian)(iA,iB,k);

						// Compute integral term
						double dUpdateA = 0.0;
						double dUpdateB = 0.0;

						for (int s = 0; s < m_nHorizontalOrder; s++) {
							dUpdateA +=
								m_dJGradientA(s,j,k)
								* dStiffness1D(i,s);

							dUpdateB +=
								m_dJGradientB(i,s,k)
								* dStiffness1D(j,s);
						}

						dUpdateA *= dInvElementDeltaA;
						dUpdateB *= dInvElementDeltaB;

						// Apply update
						(*pDataUpdate)(c,iA,iB,k) -=
							dDeltaT * dInvJacobian * dLocalNu
								* (dUpdateA + dUpdateB);
					}
					}
					}
				}
				}
			}
		}
	}
}

///////////////////////////////////////////////////////////////////////////////

void HighSpeedDynamics::ApplyVectorHyperdiffusion(
	int iDataInitial,
	int iDataWorking,
	int iDataUpdate,
	double dDeltaT,
	double dNuDiv,
	double dNuVort,
	bool fScaleNuLocally
) {
	// Variable indices
	const int UIx = 0;
	const int VIx = 1;
	const int WIx = 3;

	// Density or height index
	int RIx = 4;

	const EquationSet & eqn = m_model.GetEquationSet();
	if (eqn.GetType() == EquationSet::ShallowWaterEquations) {
		RIx = 2;
	}

	// Get a copy of the GLL grid
	GridGLL * pGrid = dynamic_cast<GridGLL*>(m_model.GetGrid());

	// Number of vertical elements
	const int nRElements = pGrid->GetRElements();

	// Apply viscosity to reference state
	bool fApplyToRefState = false;
	if (iDataInitial == DATA_INDEX_REFERENCE) {
		iDataInitial = 0;
		fApplyToRefState = true;
	}

	// Loop over all patches
	for (int n = 0; n < pGrid->GetActivePatchCount(); n++) {
		GridPatchGLL * pPatch =
			dynamic_cast<GridPatchGLL*>(pGrid->GetActivePatch(n));

		const PatchBox & box = pPatch->GetPatchBox();

		const DataArray2D<double> & dJacobian2D =
			pPatch->GetJacobian2D();
		const DataArray3D<double> & dContraMetric2DA =
			pPatch->GetContraMetric2DA();
		const DataArray3D<double> & dContraMetric2DB =
			pPatch->GetContraMetric2DB();

		DataArray4D<double> & dataInitial =
			pPatch->GetDataState(iDataInitial, DataLocation_Node);

		DataArray4D<double> & dataWorking =
			pPatch->GetDataState(iDataWorking, DataLocation_Node);

		DataArray4D<double> & dataUpdate =
			pPatch->GetDataState(iDataUpdate, DataLocation_Node);

		DataArray4D<double> & dataRef =
			pPatch->GetReferenceState(DataLocation_Node);

		// Element grid spacing and derivative coefficients
		const double dElementDeltaA = pPatch->GetElementDeltaA();
		const double dElementDeltaB = pPatch->GetElementDeltaB();

		const double dInvElementDeltaA = 1.0 / dElementDeltaA;
		const double dInvElementDeltaB = 1.0 / dElementDeltaB;

		const DataArray2D<double> & dDxBasis1D = pGrid->GetDxBasis1D();
		const DataArray2D<double> & dStiffness1D = pGrid->GetStiffness1D();

		// Compute curl and divergence of U on the grid
		DataArray3D<double> dataUa;
		dataUa.SetSize(
			dataWorking.GetSize(1),
			dataWorking.GetSize(2),
			dataWorking.GetSize(3));

		DataArray3D<double> dataUb;
		dataUb.SetSize(
			dataWorking.GetSize(1),
			dataWorking.GetSize(2),
			dataWorking.GetSize(3));

		DataArray3D<double> dataRho;
		dataRho.SetSize(
			dataInitial.GetSize(1),
			dataInitial.GetSize(2),
			dataInitial.GetSize(3));

		if (fApplyToRefState) {
			dataUa.AttachToData(&(dataRef(UIx,0,0,0)));
			dataUb.AttachToData(&(dataRef(VIx,0,0,0)));
			dataRho.AttachToData(&(dataRef(RIx,0,0,0)));
		} else {
			dataUa.AttachToData(&(dataWorking(UIx,0,0,0)));
			dataUb.AttachToData(&(dataWorking(VIx,0,0,0)));
			dataRho.AttachToData(&(dataInitial(RIx,0,0,0)));
		}

		// Compute curl and divergence of U on the grid
		pPatch->ComputeCurlAndDiv(dataUa, dataUb, dataRho);

		// Get curl and divergence
		const DataArray3D<double> & dataCurl = pPatch->GetDataVorticity();
		const DataArray3D<double> & dataDiv  = pPatch->GetDataDivergence();

		// Compute new hyperviscosity coefficient
		double dLocalNuDiv  = dNuDiv;
		double dLocalNuVort = dNuVort;

		if (fScaleNuLocally) {
			double dReferenceLength = pGrid->GetReferenceLength();
			if (dReferenceLength != 0.0) {
				dLocalNuDiv =
					dLocalNuDiv  * pow(dElementDeltaA / dReferenceLength, 3.2);
				dLocalNuVort =
					dLocalNuVort * pow(dElementDeltaA / dReferenceLength, 3.2);
			}
		}

		// Number of finite elements
		const int nElementCountA = pPatch->GetElementCountA();
		const int nElementCountB = pPatch->GetElementCountB();

		// Loop over all finite elements
		for (int a = 0; a < nElementCountA; a++) {
		for (int b = 0; b < nElementCountB; b++) {

			const int iElementA = a * m_nHorizontalOrder + box.GetHaloElements();
			const int iElementB = b * m_nHorizontalOrder + box.GetHaloElements();

			// Pointwise update of horizontal velocities
			for (int i = 0; i < m_nHorizontalOrder; i++) {
			for (int j = 0; j < m_nHorizontalOrder; j++) {
#pragma simd
			for (int k = 0; k < nRElements; k++) {

				const int iA = iElementA + i;
				const int iB = iElementB + j;

				// Compute hyperviscosity sums
				double dDaDiv = 0.0;
				double dDbDiv = 0.0;

				double dDaCurl = 0.0;
				double dDbCurl = 0.0;

				for (int s = 0; s < m_nHorizontalOrder; s++) {
					dDaDiv -=
						  dStiffness1D(i,s)
						* dataDiv(iElementA+s,iB,k);

					dDbDiv -=
						  dStiffness1D(j,s)
						* dataDiv(iA,iElementB+s,k);

					dDaCurl -=
						  dStiffness1D(i,s)
						* dataCurl(iElementA+s,iB,k);

					dDbCurl -=
						  dStiffness1D(j,s)
						* dataCurl(iA,iElementB+s,k);
				}

				dDaDiv *= dInvElementDeltaA;
				dDbDiv *= dInvElementDeltaB;

				dDaCurl *= dInvElementDeltaA;
				dDbCurl *= dInvElementDeltaB;

				// Covariant update
				const double dUpdateCovUa =
					+ dLocalNuDiv * dDaDiv
					- dLocalNuVort * dJacobian2D(iA,iB) * (
						  dContraMetric2DB(iA,iB,0) * dDaCurl
						+ dContraMetric2DB(iA,iB,1) * dDbCurl);

				const double dUpdateCovUb =
					+ dLocalNuDiv * dDbDiv
					+ dLocalNuVort * dJacobian2D(iA,iB) * (
						  dContraMetric2DA(iA,iB,0) * dDaCurl
						+ dContraMetric2DA(iA,iB,1) * dDbCurl);

				// Contravariant update
				double dUpdateConUa =
					  dContraMetric2DA(iA,iB,0) * dUpdateCovUa
					+ dContraMetric2DA(iA,iB,1) * dUpdateCovUb;

				double dUpdateConUb =
					  dContraMetric2DB(iA,iB,0) * dUpdateCovUa
					+ dContraMetric2DB(iA,iB,1) * dUpdateCovUb;

				dUpdateConUa *= dataInitial(RIx,iA,iB,k);
				dUpdateConUb *= dataInitial(RIx,iA,iB,k);

				dataUpdate(UIx,iA,iB,k) -= dDeltaT * dUpdateConUa;

				dataUpdate(VIx,iA,iB,k) -= dDeltaT * dUpdateConUb;
			}
			}
			}
		}
		}
	}
}

///////////////////////////////////////////////////////////////////////////////

//#pragma message "jeguerra: Clean up this function"

void HighSpeedDynamics::ApplyRayleighFriction(
	int iDataUpdate,
	double dDeltaT
) {
	// Get a copy of the GLL grid
	GridGLL * pGrid = dynamic_cast<GridGLL*>(m_model.GetGrid());

	// Number of vertical levels
	const int nRElements = pGrid->GetRElements();

	// Number of components to hit with friction
	int nComponents = m_model.GetEquationSet().GetComponents();

	// Equation set being solved
	int nEqSet = m_model.GetEquationSet().GetType();

	bool fCartXZ = pGrid->GetIsCartesianXZ();

	int nEffectiveC[nComponents];
	// 3D primitive nonhydro models with no density treatment
	if ((nEqSet == EquationSet::PrimitiveNonhydrostaticEquations) && !fCartXZ) {
		nEffectiveC[0] = 0; nEffectiveC[1] = 1;
		nEffectiveC[2] = 2; nEffectiveC[3] = 3;
		nEffectiveC[nComponents - 1] = 0;
		nComponents = nComponents - 1;
	}
	// 2D Cartesian XZ primitive nonhydro models with no density treatment
	else if ((nEqSet == EquationSet::PrimitiveNonhydrostaticEquations) && fCartXZ) {
		nEffectiveC[0] = 0;
		nEffectiveC[1] = 2;
		nEffectiveC[2] = 3;
		nEffectiveC[nComponents - 2] = 0;
		nEffectiveC[nComponents - 1] = 0;
		nComponents = nComponents - 2;
	}
	// Other model types (advection, shallow water, mass coord)
	else {
		for (int nc = 0; nc < nComponents; nc++) {
			nEffectiveC[nc] = nc;
		}
	}

	// Subcycle the rayleigh update
	int nRayleighCycles = 10;
	double dRayleighFactor = 1.0 / nRayleighCycles;

	// Perform local update
	for (int n = 0; n < pGrid->GetActivePatchCount(); n++) {
		GridPatchGLL * pPatch =
			dynamic_cast<GridPatchGLL*>(pGrid->GetActivePatch(n));

		const PatchBox & box = pPatch->GetPatchBox();

		// Grid data
		DataArray4D<double> & dataUpdateNode =
			pPatch->GetDataState(iDataUpdate, DataLocation_Node);

		DataArray4D<double> & dataUpdateREdge =
			pPatch->GetDataState(iDataUpdate, DataLocation_REdge);

		// Reference state
		const DataArray4D<double> & dataReferenceNode =
			pPatch->GetReferenceState(DataLocation_Node);

		const DataArray4D<double> & dataReferenceREdge =
			pPatch->GetReferenceState(DataLocation_REdge);

		// Rayleigh friction strength
		const DataArray3D<double> & dataRayleighStrengthNode =
			pPatch->GetRayleighStrength(DataLocation_Node);

		const DataArray3D<double> & dataRayleighStrengthREdge =
			pPatch->GetRayleighStrength(DataLocation_REdge);

		// Loop over all nodes in patch
		for (int i = box.GetAInteriorBegin(); i < box.GetAInteriorEnd(); i++) {
		for (int j = box.GetBInteriorBegin(); j < box.GetBInteriorEnd(); j++) {

			// Rayleigh damping on nodes
			for (int k = 0; k < nRElements; k++) {

				double dNu = dataRayleighStrengthNode(i,j,k);

				// Backwards Euler
				if (dNu == 0.0) {
					continue;
				}

				double dNuNode = 1.0 / (1.0 + dDeltaT * dNu);

				// Loop over all effective components
				for (int c = 0; c < nComponents; c++) {
					if (pGrid->GetVarLocation(nEffectiveC[c]) ==
						DataLocation_Node) {
						for (int si = 0; si < nRayleighCycles; si++) {
							dNuNode = 1.0 / (1.0 + dRayleighFactor * dDeltaT * dNu);
							dataUpdateNode(nEffectiveC[c],i,j,k) =
								dNuNode * dataUpdateNode(nEffectiveC[c],i,j,k)
								+ (1.0 - dNuNode)
								* dataReferenceNode(nEffectiveC[c],i,j,k);
						}
					}
				}
			}

			// Rayleigh damping on interfaces
			for (int k = 0; k <= nRElements; k++) {

				double dNu = dataRayleighStrengthREdge(i,j,k);

				// Backwards Euler
				if (dNu == 0.0) {
					continue;
				}

				double dNuREdge = 1.0 / (1.0 + dDeltaT * dNu);

				// Loop over all effective components
				for (int c = 0; c < nComponents; c++) {
					if (pGrid->GetVarLocation(nEffectiveC[c]) ==
						DataLocation_REdge) {
						for (int si = 0; si < nRayleighCycles; si++) {
							dNuREdge = 1.0 / (1.0 + dRayleighFactor * dDeltaT * dNu);
							dataUpdateREdge(nEffectiveC[c],i,j,k) =
							dNuREdge * dataUpdateREdge(nEffectiveC[c],i,j,k)
							+ (1.0 - dNuREdge)
							* dataReferenceREdge(nEffectiveC[c],i,j,k);
						}
					}
				}
			}
		}
		}
	}
}

///////////////////////////////////////////////////////////////////////////////

void HighSpeedDynamics::StepAfterSubCycle(
	int iDataInitial,
	int iDataUpdate,
	int iDataWorking,
	const Time & time,
	double dDeltaT
) {
	// Start the function timer
	FunctionTimer timer("StepAfterSubCycle");

	// Check indices
	if (iDataInitial == iDataWorking) {
		_EXCEPTIONT("Invalid indices "
			"-- initial and working data must be distinct");
	}
	if (iDataUpdate == iDataWorking) {
		_EXCEPTIONT("Invalid indices "
			"-- working and update data must be distinct");
	}

	// Get the GLL grid
	GridGLL * pGrid = dynamic_cast<GridGLL*>(m_model.GetGrid());

	// Copy initial data to updated data
	pGrid->CopyData(iDataInitial, iDataUpdate, DataType_State);
	pGrid->CopyData(iDataInitial, iDataUpdate, DataType_Tracers);

	// No hyperdiffusion
	if ((m_dNuScalar == 0.0) && (m_dNuDiv == 0.0) && (m_dNuVort == 0.0)) {

	// Apply hyperdiffusion
	} else if (m_nHyperviscosityOrder == 0) {

	// Apply viscosity
	} else if (m_nHyperviscosityOrder == 2) {

		// Apply scalar and vector viscosity
		ApplyScalarHyperdiffusion(
			iDataInitial, iDataUpdate, dDeltaT, m_dNuScalar, false);
		ApplyVectorHyperdiffusion(
			iDataInitial, iDataInitial, iDataUpdate, -dDeltaT, m_dNuDiv, m_dNuVort, false);

		// Apply positive definite filter to tracers
		FilterNegativeTracers(iDataUpdate);

		// Apply Direct Stiffness Summation
		pGrid->ApplyDSS(iDataUpdate, DataType_State);
		pGrid->ApplyDSS(iDataUpdate, DataType_Tracers);

	// Apply hyperviscosity
	} else if (m_nHyperviscosityOrder == 4) {

		// Apply scalar and vector hyperviscosity (first application)
		pGrid->ZeroData(iDataWorking, DataType_State);
		pGrid->ZeroData(iDataWorking, DataType_Tracers);

		ApplyScalarHyperdiffusion(
			iDataInitial, iDataWorking, 1.0, 1.0, false);
		ApplyVectorHyperdiffusion(
			iDataInitial, iDataInitial, iDataWorking, 1.0, 1.0, 1.0, false);

		// Apply Direct Stiffness Summation
		pGrid->ApplyDSS(iDataWorking, DataType_State);
		pGrid->ApplyDSS(iDataWorking, DataType_Tracers);

		// Apply scalar and vector hyperviscosity (second application)
		ApplyScalarHyperdiffusion(
			iDataWorking, iDataUpdate, -dDeltaT, m_dNuScalar, true);
		ApplyVectorHyperdiffusion(
			iDataInitial, iDataWorking, iDataUpdate, -dDeltaT, m_dNuDiv, m_dNuVort, true);

		// Apply positive definite filter to tracers
		FilterNegativeTracers(iDataUpdate);

		// Apply Direct Stiffness Summation
		pGrid->ApplyDSS(iDataUpdate, DataType_State);
		pGrid->ApplyDSS(iDataUpdate, DataType_Tracers);

	// Invalid viscosity order
	} else {
		_EXCEPTIONT("Invalid viscosity order");
	}

#ifdef APPLY_RAYLEIGH_WITH_HYPERVIS
		// Apply Rayleigh damping
		if (pGrid->HasRayleighFriction()) {
			ApplyRayleighFriction(iDataUpdate, dDeltaT);
		}
#endif
}

///////////////////////////////////////////////////////////////////////////////