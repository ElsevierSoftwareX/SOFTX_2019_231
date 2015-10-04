/**
 * @file CollisionDB.cpp
 *
 * @brief Implementation of CollisionDB type.
 */

/*
 * Copyright 2014-2015 von Karman Institute for Fluid Dynamics (VKI)
 *
 * This file is part of MUlticomponent Thermodynamic And Transport
 * properties for IONized gases in C++ (Mutation++) software package.
 *
 * Mutation++ is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Mutation++ is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Mutation++.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include "CollisionDBNew.h"
#include "Species.h"
#include "Thermodynamics.h"
#include "XMLite.h"
#include "Utilities.h"
using namespace Mutation::Thermodynamics;
using namespace Mutation::Utilities;
using namespace Mutation::Utilities::IO;

#include <cassert>
#include <fstream>
#include <iostream>
using namespace std;

namespace Mutation {
    namespace Transport {

//==============================================================================

CollisionDBNew::CollisionDBNew(
    const string& db_name, const Thermodynamics::Thermodynamics& thermo) :
    m_database(databaseFileName(db_name, "transport")),
    m_thermo(thermo),
    m_tabulate(true), m_table_min(300.),  m_table_max(20000.), m_table_del(100.),
    m_etai(thermo.nHeavy()), m_etafac(thermo.nHeavy()),
    m_nDei(thermo.nSpecies()*(thermo.hasElectrons() ? 1 : 0)),
    m_Deifac(thermo.nSpecies()*(thermo.hasElectrons() ? 1 : 0)),
    m_nDij(thermo.nHeavy()*(thermo.nHeavy()+1)/2),
    m_Dijfac(thermo.nHeavy()*(thermo.nHeavy()+1)/2),
    m_Dim(thermo.nSpecies())
{
    XmlElement& root = m_database.root();

    // Determine if we should tabulate collision integrals when possible
    root.getAttribute("tabulate", m_tabulate, m_tabulate);
    root.getAttribute("Tmin", m_table_min, m_table_min);
    root.getAttribute("Tmax", m_table_max, m_table_max);
    root.getAttribute("dT",   m_table_del, m_table_del);

    // Check the table data
    if (m_tabulate) {
        root.parseCheck(m_table_min > 0.0, "Tmin must be positive.");
        root.parseCheck(m_table_max > 0.0, "Tmax must be positive.");
        root.parseCheck(m_table_del > 0.0, "dT must be positive.");
        root.parseCheck(m_table_min < m_table_max, "Tmin must be > Tmax.");

        double size = (m_table_max-m_table_min)/m_table_del;
        root.parseCheck(std::abs(size-int(size))/size < 1.0e-15,
            "(Tmax - Tmin)/dT must be a positive whole number.");
    }

    // Loop over the species and create the list of species pairs
    const vector<Species>& species = m_thermo.species();
    for (int i = 0; i < species.size(); ++i)
        for (int j = i; j < species.size(); ++j)
            m_pairs.push_back(CollisionPairNew(species[i], species[j], &root));

    // Compute eta factors
    const int k = thermo.nSpecies() - thermo.nHeavy();
    for (int i = k; i < thermo.nSpecies(); ++i)
        m_etafac[i-k] = 5./16.*std::sqrt(PI*RU*thermo.speciesMw(i));

    // Compute the nDei factors
    if (k > 0) {
        for (int i = 0; i < thermo.nSpecies(); ++i)
            m_Deifac(i) = 3./16.*std::sqrt(TWOPI*RU/thermo.speciesMw(0));
        m_Deifac(0) *= 2./SQRT2;
    }

    // Compute the nDij factors
    for (int i = k, index = 0; i < thermo.nSpecies(); ++i)
        for (int j = i; j < thermo.nSpecies(); ++j, index++)
            m_Dijfac(index) = 3./16.*std::sqrt(TWOPI*RU*
                (thermo.speciesMw(i)+thermo.speciesMw(j))/
                (thermo.speciesMw(i)*thermo.speciesMw(j)));
}

//==============================================================================

CollisionDBNew::GroupType CollisionDBNew::groupType(const string& name)
{
    const int len = name.length();
    if (name[len-2] == 'e') {
        if (name [len-1] == 'e') return EE;
        if (name [len-1] == 'i') return EI;
    }
    else if (name[len-2] == 'i') {
        if (name [len-1] == 'i') return II;
        if (name [len-1] == 'j') return IJ;
    }
    return BAD_TYPE;
}

//==============================================================================


const CollisionGroup& CollisionDBNew::group(const string& name)
{
    // Minimal check on the string argument
    GroupType type = groupType(name);
    assert(type != BAD_TYPE);

    // Check if this group is already being managed
    map<string, CollisionGroup>::iterator iter = m_groups.find(name);
    if (iter != m_groups.end()) return iter->second.update(
        (type < II ? m_thermo.Te() : m_thermo.T()), m_thermo);

    // Create a new group to manage this type
    CollisionGroup& new_group = m_groups.insert(
        make_pair(name, CollisionGroup(
            m_tabulate, m_table_min, m_table_max, m_table_del))).first->second;

    // Determine start and end iterator for this group
    const int ns = m_thermo.nSpecies();
    const int e  = (m_thermo.hasElectrons() ? 1 : 0);
    const int k  = e*ns;

    std::vector<CollisionPairNew>::iterator start = m_pairs.begin();
    std::vector<CollisionPairNew>::iterator end   = m_pairs.end();
    std::vector<CollisionPairNew> diag;

    switch (type) {
    case EE: end = start + e; break;
    case EI: end = start + k; break;
    case IJ: start += k; break;
    case II:
        // create a temporary list of the heavy diagonal components
        for (int i = 0, index = k; i < ns-e; index += ns-e-i, i++)
            diag.push_back(m_pairs[index]);
        start = diag.begin();
        end   = diag.end();
        break;
    default:
        cout << "Bad collision integral group type: '"
             << name.substr(name.length()-2) << "' in group name: '"
             << name << "'." << endl;
        cout << "Allowed group types are 'ee', 'ei', 'ii', and 'ij'." << endl;
        exit(1);
    }

    // Manage the collision integrals
    string kind = name.substr(0, name.length()-2);
    new_group.manage(start, end, &CollisionPairNew::get, kind);

    // Compute integrals and return the group
    return new_group.update(
        (type < II ? m_thermo.Te() : m_thermo.T()), m_thermo);
}

//==============================================================================

const Eigen::ArrayXd& CollisionDBNew::etai()
{
    return (m_etai = std::sqrt(m_thermo.T()) * m_etafac / Q22ii());
}

//==============================================================================

const Eigen::ArrayXd& CollisionDBNew::nDei()
{
    if (m_nDei.size() > 0)
        m_nDei = std::sqrt(m_thermo.Te()) * m_Deifac / Q11ei();
    return m_nDei;
}

//==============================================================================

const Eigen::ArrayXd& CollisionDBNew::nDij()
{
    return (m_nDij = std::sqrt(m_thermo.T()) * m_Dijfac / Q11ij().array());
}

//==============================================================================

const Eigen::ArrayXd& CollisionDBNew::Dim()
{
    return (m_Dim = Eigen::ArrayXd::Zero(m_thermo.nSpecies()));
}

//==============================================================================

    } // namespace Transport
} // namespace Mutation

