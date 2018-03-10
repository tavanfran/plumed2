/* +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
   Copyright (c) 2016,2017 The plumed team
   (see the PEOPLE file at the root of the distribution for a list of names)

   See http://www.plumed.org for more information.

   This file is part of plumed, version 2.

   plumed is free software: you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   plumed is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with plumed.  If not, see <http://www.gnu.org/licenses/>.
+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++ */
#include "core/ActionAtomistic.h"
#include "core/ActionWithValue.h"
#include "core/ActionWithArguments.h"
#include "core/ActionRegister.h"
#include "tools/KernelFunctions.h"
#include "tools/RootFindingBase.h"

//+PLUMEDOC COLVAR DISTANCE_FROM_CONTOUR
/*
Calculate the perpendicular distance from a Willard-Chandler dividing surface.

Suppose that you have calculated a multicolvar.  By doing so you have calculated a
set of colvars, \f$s_i\f$, and each of these colvars has a well defined position in
space \f$(x_i,y_i,z_i)\f$.  You can use this information to calculate a phase-field
model of the colvar density using:

\f[
p(x,y,x) = \sum_{i} s_i K\left[\frac{x-x_i}{\sigma_x},\frac{y-y_i}{\sigma_y},\frac{z-z_i}{\sigma_z} \right]
\f]

In this expression \f$\sigma_x, \sigma_y\f$ and \f$\sigma_z\f$ are bandwidth parameters and
\f$K\f$ is one of the \ref kernelfunctions.  This is what is done within \ref MULTICOLVARDENS

The Willard-Chandler surface is a surface of constant density in the above phase field \f$p(x,y,z)\f$.
In other words, it is a set of points, \f$(x',y',z')\f$, in your box which have:

\f[
p(x',y',z') = \rho
\f]

where \f$\rho\f$ is some target density.  This action caculates the distance projected on the \f$x, y\f$ or
\f$z\f$ axis between the position of some test particle and this surface of constant field density.

\par Examples

In this example atoms 2-100 are assumed to be concentraed along some part of the \f$z\f$ axis so that you
an interface between a liquid/solid and the vapour.  The quantity dc measures the distance between the
surface at which the density of 2-100 atoms is equal to 0.2 and the position of the test particle atom 1.

\plumedfile
dens: DENSITY SPECIES=2-100
dc: DISTANCE_FROM_CONTOUR DATA=dens ATOM=1 BANDWIDTH=0.5,0.5,0.5 DIR=z CONTOUR=0.2
\endplumedfile

*/
//+ENDPLUMEDOC

namespace PLMD {
namespace multicolvar {

class DistanceFromContour :
  public ActionWithValue,
  public ActionAtomistic,
  public ActionWithArguments
{
private:
  unsigned dir;
  double rcut2;
  double contour;
  double pbc_param;
  std::string kerneltype;
  std::vector<Value*> pval;
  std::vector<double> bw, pos1, pos2, dirv, dirv2;
  std::vector<double> forces;
  std::vector<unsigned> perp_dirs;
  unsigned nactive;
  std::vector<unsigned> active_list;
  std::vector<Vector> atom_deriv;
  std::vector<double> forcesToApply;
  RootFindingBase<DistanceFromContour> mymin;
public:
  static void registerKeywords( Keywords& keys );
  explicit DistanceFromContour( const ActionOptions& );
  ~DistanceFromContour();
  unsigned getNumberOfDerivatives() const ;
  void lockRequests();
  void unlockRequests();
  void calculateNumericalDerivatives( ActionWithValue* a ) { plumed_merror("numerical derivatives are not implemented for this action"); }
  void calculate();
  void evaluateDerivatives( const Vector root1, const double& root2 );
  double getDifferenceFromContour( const std::vector<double>& x, std::vector<double>& der );
// We need an apply action as we are using an independent value
  void apply();
};

PLUMED_REGISTER_ACTION(DistanceFromContour,"DISTANCE_FROM_CONTOUR")

void DistanceFromContour::registerKeywords( Keywords& keys ) {
  Action::registerKeywords( keys ); ActionWithValue::registerKeywords( keys );
  ActionAtomistic::registerKeywords( keys ); ActionWithArguments::registerKeywords( keys );
  keys.remove("NUMERICAL_DERIVATIVES");
  keys.addOutputComponent("dist1","default","the distance between the reference atom and the nearest contour");
  keys.addOutputComponent("dist2","default","the distance between the reference atom and the other contour");
  keys.addOutputComponent("qdist","default","the differentiable (squared) distance between the two contours (see above)");
  keys.addOutputComponent("thickness","default","the distance between the two contours on the line from the reference atom");
  keys.add("atoms","POSITIONS","the positions of the atoms that we are calculating the contour from");
  keys.add("atoms","ATOM","The atom whose perpendicular distance we are calculating from the contour");
  keys.add("compulsory","BANDWIDTH","the bandwidths for kernel density esimtation");
  keys.add("compulsory","KERNEL","gaussian","the kernel function you are using.  More details on  the kernels available "
           "in plumed plumed can be found in \\ref kernelfunctions.");
  keys.add("compulsory","DIR","the direction perpendicular to the contour that you are looking for");
  keys.add("compulsory","CONTOUR","the value we would like for the contour");
  keys.add("compulsory","TOLERANCE","0.1","this parameter is used to manage periodic boundary conditions.  The problem "
           "here is that we can be between contours even when we are not within the membrane "
           "because of periodic boundary conditions.  When we are in the contour, however, we "
           "should have it so that the sums of the absoluate values of the distances to the two "
           "contours is approximately the distance between the two contours.  There can be numerical errors in these calculations, however, so "
           "we specify a small tolerance here");
}

DistanceFromContour::DistanceFromContour( const ActionOptions& ao ):
  Action(ao),
  ActionWithValue(ao),
  ActionAtomistic(ao),
  ActionWithArguments(ao),
  bw(3),
  pos1(3,0.0),
  pos2(3,0.0),
  dirv(3,0.0),
  dirv2(3,0.0),
  perp_dirs(2),
  nactive(0),
  mymin(this)
{
  if( getNumberOfArguments()>1 ) error("should only use one argument for this action");
  if( getNumberOfArguments()==1 ) {
    if( getPntrToArgument(0)->getRank()!=1 ) error("ARG for distance from contour should be rank one");
  }
  // Read in the multicolvar/atoms
  std::vector<AtomNumber> atoms; parseAtomList("POSITIONS",atoms);
  std::vector<AtomNumber> origin; parseAtomList("ATOM",origin);
  if( origin.size()!=1 ) error("should only specify one atom for origin keyword");

  log.printf("  calculating distance between atom %d and contour \n", origin[0].serial() );
  log.printf("  contour is in field constructed from positions of atoms : ");
  for(unsigned i=0; i<atoms.size(); ++i) log.printf("%d ",atoms[i].serial() );
  if( getNumberOfArguments()==1 ) {
    if( getPntrToArgument(0)->getShape()[0]!=atoms.size() ) error("mismatch between number of atoms and size of vector specified using ARG keyword");
    log.printf("\n  and weights from %s \n", getPntrToArgument(0)->getName().c_str() );
  } else {
    log.printf("\n  all weights are set equal to one \n");
  }
  // Request everything we need
  active_list.resize( atoms.size(), 0 ); atom_deriv.resize( atoms.size() );
  std::vector<Value*> args( getArguments() ); atoms.push_back( origin[0] );
  requestAtoms( atoms ); requestArguments( args, false );

  // Get the direction
  std::string ldir; parse("DIR",ldir );
  if( ldir=="x" ) { dir=0; perp_dirs[0]=1; perp_dirs[1]=2; dirv[0]=1; dirv2[0]=-1; }
  else if( ldir=="y" ) { dir=1; perp_dirs[0]=0; perp_dirs[1]=2; dirv[1]=1; dirv2[1]=-1; }
  else if( ldir=="z" ) { dir=2; perp_dirs[0]=0; perp_dirs[1]=1; dirv[2]=1; dirv2[2]=-1; }
  else error(ldir + " is not a valid direction use x, y or z");

  // Read in details of phase field construction
  parseVector("BANDWIDTH",bw); parse("KERNEL",kerneltype); parse("CONTOUR",contour);
  log.printf("  constructing phase field using %s kernels with bandwidth (%f, %f, %f) \n",kerneltype.c_str(), bw[0], bw[1], bw[2] );
  // Read in the tolerance for the pbc parameter
  std::vector<AtomNumber> all_atoms; parse("TOLERANCE",pbc_param);

  // And a cutoff
  std::vector<double> pp( bw.size(),0 );
  KernelFunctions kernel( pp, bw, kerneltype, "DIAGONAL", 1.0 );
  double rcut = kernel.getCutoff( bw[0] );
  for(unsigned j=1; j<bw.size(); ++j) {
    if( kernel.getCutoff(bw[j])>rcut ) rcut=kernel.getCutoff(bw[j]);
  }
  rcut2=rcut*rcut; std::vector<unsigned> shape;
  // Create the values
  addComponent("thickness", shape ); componentIsNotPeriodic("thickness");
  addComponent("dist1", shape ); componentIsNotPeriodic("dist1");
  addComponent("dist2", shape ); componentIsNotPeriodic("dist2");
  addComponentWithDerivatives("qdist", shape ); componentIsNotPeriodic("qdist");

  // Create the vector of values that holds the position
  for(unsigned i=0; i<3; ++i) pval.push_back( new Value() );
  forcesToApply.resize( 3*getNumberOfAtoms() + 9 );
}

DistanceFromContour::~DistanceFromContour() {
  for(unsigned i=0; i<3; ++i) delete pval[i];
}

void DistanceFromContour::lockRequests() {
  ActionWithArguments::lockRequests();
  ActionAtomistic::lockRequests();
}

void DistanceFromContour::unlockRequests() {
  ActionWithArguments::unlockRequests();
  ActionAtomistic::unlockRequests();
}

unsigned DistanceFromContour::getNumberOfDerivatives() const {
  if( getNumberOfArguments()==1 ) return 4*getNumberOfAtoms() + 8;  // One derivative for each weight hence four times the number of atoms - 1
  return 3*getNumberOfAtoms() + 9;
}

void DistanceFromContour::calculate() {
  // Check box is orthorhombic
  if( !getPbc().isOrthorombic() ) error("cell box must be orthorhombic");

  // The nanoparticle is at the origin of our coordinate system
  pos1[0]=pos1[1]=pos1[2]=0.0; pos2[0]=pos2[1]=pos2[2]=0.0;

  // Set bracket as center of mass of membrane in active region
  Vector myvec = pbcDistance( getPosition(getNumberOfAtoms()-1), getPosition(0) ); pos2[dir]=myvec[dir];
  nactive=1; active_list[0]=0; double d2, mindist = myvec.modulo2();
  for(unsigned j=1; j<getNumberOfAtoms()-1; ++j) {
    Vector distance=pbcDistance( getPosition(getNumberOfAtoms()-1), getPosition(j) );
    if( (d2=distance[perp_dirs[0]]*distance[perp_dirs[0]])<rcut2 &&
        (d2+=distance[perp_dirs[1]]*distance[perp_dirs[1]])<rcut2 ) {
      d2+=distance[dir]*distance[dir];
      if( d2<mindist && fabs(distance[dir])>epsilon ) { pos2[dir]=distance[dir]; mindist = d2; }
      active_list[nactive]=j; nactive++;
    }
  }
  // pos1 position of the nanoparticle, in the first time
  // pos2 is the position of the closer atom in the membrane with respect the nanoparticle
  // fa = distance between pos1 and the contour
  // fb = distance between pos2 and the contour
  std::vector<double> faked(3);
  double fa = getDifferenceFromContour( pos1, faked );
  double fb = getDifferenceFromContour( pos2, faked );
  if( fa*fb>0 ) {
    unsigned maxtries = std::floor( ( getBox()(dir,dir) ) / bw[dir] );
    for(unsigned i=0; i<maxtries; ++i) {
      double sign=(pos2[dir]>0)? -1 : +1; // If the nanoparticle is inside the membrane push it out
      pos1[dir] += sign*bw[dir]; fa = getDifferenceFromContour( pos1, faked );
      if( fa*fb<0 ) break;
      // if fa*fb is less than zero the new pos 1 is outside the contour
    }
  }
  // Set direction for contour search
  dirv[dir] = pos2[dir] - pos1[dir];
  // Bracket for second root starts in center of membrane
  double fc = getDifferenceFromContour( pos2, faked );
  if( fc*fb>0 ) {
    // first time is true, because fc=fb
    // push pos2 from its initial position inside the membrane towards the second contourn
    unsigned maxtries = std::floor( ( getBox()(dir,dir) ) / bw[dir] );
    for(unsigned i=0; i<maxtries; ++i) {
      double sign=(dirv[dir]>0)? +1 : -1;
      pos2[dir] += sign*bw[dir]; fc = getDifferenceFromContour( pos2, faked );
      if( fc*fb<0 ) break;
    }
    dirv2[dir] = ( pos1[dir] + dirv[dir] ) - pos2[dir];
  }

  // Now do a search for the two contours
  mymin.lsearch( dirv, pos1, &DistanceFromContour::getDifferenceFromContour );
  // Save the first value
  Vector root1; root1.zero(); root1[dir] = pval[dir]->get();
  mymin.lsearch( dirv2, pos2, &DistanceFromContour::getDifferenceFromContour );
  // Calculate the separation between the two roots using PBC
  Vector root2; root2.zero(); root2[dir]=pval[dir]->get();
  Vector sep = pbcDistance( root1, root2 ); double spacing = fabs( sep[dir] ); plumed_assert( spacing>epsilon );
  getPntrToComponent("thickness")->set( spacing );

  // Make sure the sign is right
  double predir=(root1[dir]*root2[dir]<0)? -1 : 1;
  // This deals with periodic boundary conditions - if we are inside the membrane the sum of the absolute
  // distances from the contours should add up to the spacing.  When this is not the case we must be outside
  // the contour
  // if( predir==-1 && (fabs(root1[dir])+fabs(root2[dir]))>(spacing+pbc_param) ) predir=1;
  // Set the final value to root that is closest to the "origin" = position of atom
  if( fabs(root1[dir])<fabs(root2[dir]) ) {
    getPntrToComponent("dist1")->set( predir*fabs(root1[dir]) );
    getPntrToComponent("dist2")->set( fabs(root2[dir]) );
  } else {
    getPntrToComponent("dist1")->set( predir*fabs(root2[dir]) );
    getPntrToComponent("dist2")->set( fabs(root1[dir]) );
  }
  getPntrToComponent("qdist")->set( root2[dir]*root1[dir] );

  // Now calculate the derivatives
  if( !doNotCalculateDerivatives() ) {
    evaluateDerivatives( root1, root2[dir] ); evaluateDerivatives( root2, root1[dir] );
  }
}

void DistanceFromContour::evaluateDerivatives( const Vector root1, const double& root2 ) {
  if( getNumberOfArguments()>0 ) plumed_merror("derivatives for phase field distance from contour have not been implemented yet");
  for(unsigned j=0; j<3; ++j) pval[j]->set( root1[j] );

  Vector origind; origind.zero(); Tensor vir; vir.zero();
  double sumd = 0; std::vector<double> pp(3), ddd(3,0);
  for(unsigned i=0; i<nactive; ++i) {
    Vector distance = pbcDistance( getPosition(getNumberOfAtoms()-1), getPosition(active_list[i]) );
    for(unsigned j=0; j<3; ++j) pp[j] = distance[j];

    // Now create the kernel and evaluate
    KernelFunctions kernel( pp, bw, kerneltype, false, 1.0, true );
    double newval = kernel.evaluate( pval, ddd, true );
    if( getNumberOfArguments()==1 ) {
    } else {
      sumd += ddd[dir];
      for(unsigned j=0; j<3; ++j) atom_deriv[i][j] = -ddd[j];
      origind += -atom_deriv[i]; vir -= Tensor(atom_deriv[i],distance);
    }
  }

  // Add derivatives to atoms involved
  Value* val=getPntrToComponent("qdist"); double prefactor =  root2 / sumd;
  for(unsigned i=0; i<nactive; ++i) {
    val->addDerivative( 3*active_list[i] + 0, -prefactor*atom_deriv[i][0] );
    val->addDerivative( 3*active_list[i] + 1, -prefactor*atom_deriv[i][1] );
    val->addDerivative( 3*active_list[i] + 2, -prefactor*atom_deriv[i][2] );
  }

  // Add derivatives to atoms at origin
  unsigned nbase = 3*(getNumberOfAtoms()-1);
  val->addDerivative( nbase, -prefactor*origind[0] ); nbase++;
  val->addDerivative( nbase, -prefactor*origind[1] ); nbase++;
  val->addDerivative( nbase, -prefactor*origind[2] ); nbase++;

  // Add derivatives to virial
  for(unsigned i=0; i<3; ++i) for(unsigned j=0; j<3; ++j) { val->addDerivative( nbase, -prefactor*vir(i,j) ); nbase++; }
}

double DistanceFromContour::getDifferenceFromContour( const std::vector<double>& x, std::vector<double>& der ) {
  std::string min, max;
  for(unsigned j=0; j<3; ++j) {
    Tools::convert( -0.5*getBox()(j,j), min );
    Tools::convert( +0.5*getBox()(j,j), max );
    pval[j]->setDomain( min, max ); pval[j]->set( x[j] );
  }
  double sumk = 0, sumd = 0; std::vector<double> pp(3), ddd(3,0);
  for(unsigned i=0; i<nactive; ++i) {
    Vector distance = pbcDistance( getPosition(getNumberOfAtoms()-1), getPosition(active_list[i]) );
    for(unsigned j=0; j<3; ++j) pp[j] = distance[j];

    // Now create the kernel and evaluate
    KernelFunctions kernel( pp, bw, kerneltype, false, 1.0, true );
    double newval = kernel.evaluate( pval, ddd, true );
    if( getNumberOfArguments()==1 ) {
      sumk += getPntrToArgument(0)->get(active_list[i])*newval;
      sumd += newval;
    } else sumk += newval;
  }
  if( getNumberOfArguments()==0 ) return sumk - contour;
  return (sumk/sumd) - contour;
}

void DistanceFromContour::apply() {
  if( doNotCalculateDerivatives() ) return ;
  std::vector<Vector>&   f(modifyForces());
  Tensor&           v(modifyVirial());
  const unsigned    nat=getNumberOfAtoms();

  std::fill(forcesToApply.begin(),forcesToApply.end(),0);
  if(getPntrToComponent(3)->applyForce(forcesToApply)) {
    for(unsigned j=0; j<nat; ++j) {
      f[j][0]+=forcesToApply[3*j+0];
      f[j][1]+=forcesToApply[3*j+1];
      f[j][2]+=forcesToApply[3*j+2];
    }
    v(0,0)+=forcesToApply[3*nat+0];
    v(0,1)+=forcesToApply[3*nat+1];
    v(0,2)+=forcesToApply[3*nat+2];
    v(1,0)+=forcesToApply[3*nat+3];
    v(1,1)+=forcesToApply[3*nat+4];
    v(1,2)+=forcesToApply[3*nat+5];
    v(2,0)+=forcesToApply[3*nat+6];
    v(2,1)+=forcesToApply[3*nat+7];
    v(2,2)+=forcesToApply[3*nat+8];
  }
}

}
}
