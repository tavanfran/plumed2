/* +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
   Copyright (c) 2015,2016 The plumed team
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
/* 
 This class was originally written by Alexander Jussupow and
 Carlo Camilloni 
*/

#include "Colvar.h"
#include "ActionRegister.h"
#include "core/ActionSet.h"
#include "core/PlumedMain.h"
#include "core/SetupMolInfo.h"
#include "tools/Communicator.h"
#include "tools/OpenMP.h"

#include <string>
#include <cmath>

using namespace std;

namespace PLMD{
namespace colvar{

//+PLUMEDOC COLVAR SAXS
/*

Calculate SAXS scattered intensity

*/
//+ENDPLUMEDOC
   
class SAXS : public Colvar {
private:
  bool                     pbc;
  bool                     serial;
  unsigned                 numq;
  vector<double>           q_list;
  vector<vector<double> >  FF_value;
  vector<double>           FF_rank;

public:
  static void registerKeywords( Keywords& keys );
  explicit SAXS(const ActionOptions&);
  void getStructureFactors(const vector<AtomNumber> &atoms, vector<vector<long double> > &parameter);
  virtual void calculate();
};

PLUMED_REGISTER_ACTION(SAXS,"SAXS")

void SAXS::registerKeywords(Keywords& keys){
  Colvar::registerKeywords( keys );
  componentsAreNotOptional(keys);
  useCustomisableComponents(keys);
  keys.addFlag("SERIAL",false,"Perform the calculation in serial - for debug purpose");
  keys.add("atoms","ATOMS","The atoms to be included in the calculation, e.g. the whole protein.");
  keys.add("compulsory","NUMQ","Number of used q values");
  keys.add("numbered","QVALUE","Used qvalue Keywords like QVALUE1, QVALUE2, to list the scattering length to calculate SAXS.");
  keys.add("numbered","PARAMETERS","Used parameter Keywords like PARAMETERS1, PARAMETERS2. These are used to calculate the structure factor for the i-th atom/bead.");
  keys.addFlag("ADDEXPVALUES",false,"Set to TRUE if you want to have fixed components with the experimental values.");
  keys.add("numbered","EXPINT","Add an experimental value for each q value.");
  keys.add("compulsory","SCEXP","SCALING value of the experimental data. Usefull to simplify the comparison.");
}

SAXS::SAXS(const ActionOptions&ao):
PLUMED_COLVAR_INIT(ao),
pbc(true),
serial(false)
{
  vector<AtomNumber> atoms;
  parseAtomList("ATOMS",atoms);
  const unsigned size = atoms.size();

  parseFlag("SERIAL",serial);

  bool nopbc=!pbc;
  parseFlag("NOPBC",nopbc);
  pbc=!nopbc;

  numq = 0;
  parse("NUMQ",numq);
  if(numq==0) error("NUMQ must be set");  

  double scexp = 0;
  parse("SCEXP",scexp);
  if(scexp==0) error("SCEXP must be set");  

  q_list.resize( numq );
  unsigned ntarget=0;
  for(unsigned i=0;i<numq;++i){
    if( !parseNumbered( "QVALUE", i+1, q_list[i]) ) break;
    ntarget++;
  }
  if( ntarget!=numq ) error("found wrong number of qvalue values");

  for(unsigned i=0;i<numq;i++) {
    log.printf("  my q: %lf \n",q_list[i]);    
    std::string num; Tools::convert(i,num);
    addComponentWithDerivatives("q_"+num);
    componentIsNotPeriodic("q_"+num);
  }

  //read in parameter vector
  vector<vector<long double> > parameter;
  parameter.resize(size);
  ntarget=0;
  for(unsigned i=0;i<size;++i){
    if( !parseNumberedVector( "PARAMETERS", i+1, parameter[i]) ) break;
    ntarget++;
  }
  if( ntarget==0 ) getStructureFactors(atoms, parameter);
  else if( ntarget!=size ) error("found wrong number of parameter vectors");

  FF_value.resize(numq,vector<double>(size));
  vector<vector<long double> >  FF_tmp;
  FF_tmp.resize(numq,vector<long double>(size));
  for(unsigned i=0;i<size;++i) {
    for(unsigned j=0;j<parameter[i].size();++j) {
      for(unsigned k=0;k<numq;++k){
        FF_tmp[k][i]+= parameter[i][j]*powl(static_cast<long double>(q_list[k]),j);
      }
    }
  }

  // Calculate Rank of FF_matrix
  FF_rank.resize(numq);
  for(unsigned k=0;k<numq;++k){
    for(unsigned i=0;i<size;i++){
       FF_value[k][i] = static_cast<double>(FF_tmp[k][i]);
       FF_rank[k]+=FF_value[k][i]*FF_value[k][i];
    }
  }

  bool exp=false;
  parseFlag("ADDEXPVALUES",exp);
  if(exp) {
    vector<double>   expint;
    expint.resize( numq ); 
    unsigned ntarget=0;
    for(unsigned i=0;i<numq;++i){
       if( !parseNumbered( "EXPINT", i+1, expint[i] ) ) break;
       ntarget++; 
    }
    if( ntarget!=numq ) error("found wrong number of NOEDIST values");

    for(unsigned i=0;i<numq;i++) {
      std::string num; Tools::convert(i,num);
      addComponent("exp_"+num);
      componentIsNotPeriodic("exp_"+num);
      Value* comp=getPntrToComponent("exp_"+num); comp->set(expint[i]*scexp);
    }
  }

  // convert units to nm^-1
  for(unsigned i=0;i<numq;++i){
    q_list[i]=q_list[i]*10.0;    //factor 10 to convert from A^-1 to nm^-1
  } 

  requestAtoms(atoms);
  checkRead();
}

void SAXS::getStructureFactors(const vector<AtomNumber> &atoms, vector<vector<long double> > &parameter)
{
  vector<SetupMolInfo*> moldat=plumed.getActionSet().select<SetupMolInfo*>();
  if( moldat.size()==1 ){
    log<<"  MOLINFO DATA found, using proper atom names\n";
    for(unsigned i=0;i<atoms.size();++i){
      string Aname = moldat[0]->getAtomName(atoms[i]);
      string Rname = moldat[0]->getResidueName(atoms[i]);
      if(Rname=="ALA") {
        if(Aname=="BB") {
          parameter[i].push_back(9.045);
          parameter[i].push_back(-0.098114);
          parameter[i].push_back(7.54281);
          parameter[i].push_back(-1.97438);
          parameter[i].push_back(-8.32689);
          parameter[i].push_back(6.09318);
          parameter[i].push_back(-1.18913);
        } else error("Atom name not known");
      } else if(Rname=="ARG") {
        if(Aname=="BB") {
          parameter[i].push_back(10.729);
          parameter[i].push_back(-0.0392574);
          parameter[i].push_back(1.15382);
          parameter[i].push_back(-0.155999);
          parameter[i].push_back(-2.43619);
          parameter[i].push_back(1.72922);
          parameter[i].push_back(-0.33799);
        } else if(Aname=="SC1"){
          parameter[i].push_back(-2.797);
          parameter[i].push_back(0.472403);
          parameter[i].push_back(8.07424);
          parameter[i].push_back(4.37299);
          parameter[i].push_back(-10.7398);
          parameter[i].push_back(4.95677);
          parameter[i].push_back(-0.725797);
        } else if(Aname=="SC2"){
          parameter[i].push_back(15.396);
          parameter[i].push_back(0.0636736);
          parameter[i].push_back(-1.258);
          parameter[i].push_back(1.93135);
          parameter[i].push_back(-4.45031);
          parameter[i].push_back(2.49356);
          parameter[i].push_back(-0.410721);
        } else error("Atom name not known");
      } else if(Rname=="ASN") {
        if(Aname=="BB") {
          parameter[i].push_back(10.738);
          parameter[i].push_back(-0.0402162);
          parameter[i].push_back(1.03007);
          parameter[i].push_back(-0.254174);
          parameter[i].push_back(-2.12015);
          parameter[i].push_back(1.55535);
          parameter[i].push_back(-0.30963);
        } else if(Aname=="SC1"){
          parameter[i].push_back(9.249);
          parameter[i].push_back(-0.0148678);
          parameter[i].push_back(5.52169);
          parameter[i].push_back(0.00853212);
          parameter[i].push_back(-6.71992);
          parameter[i].push_back(3.93622);
          parameter[i].push_back(-0.64973);
        } else error("Atom name not known");
      } else if(Rname=="ASP") {
        if(Aname=="BB") {
          parameter[i].push_back(10.695);
          parameter[i].push_back(-0.0410247);
          parameter[i].push_back(1.03656);
          parameter[i].push_back(-0.298558);
          parameter[i].push_back(-2.06064);
          parameter[i].push_back(1.53495);
          parameter[i].push_back(-0.308365);
        } else if(Aname=="SC1"){
          parameter[i].push_back(9.476);
          parameter[i].push_back(-0.0254664);
          parameter[i].push_back(5.57899);
          parameter[i].push_back(-0.395027);
          parameter[i].push_back(-5.9407);
          parameter[i].push_back(3.48836);
          parameter[i].push_back(-0.569402);
        } else error("Atom name not known");
      } else if(Rname=="CYS") {
        if(Aname=="BB") {
          parameter[i].push_back(10.698);
          parameter[i].push_back(-0.0233493);
          parameter[i].push_back(1.18257);
          parameter[i].push_back(0.0684463);
          parameter[i].push_back(-2.792);
          parameter[i].push_back(1.88995);
          parameter[i].push_back(-0.360229);
        } else if(Aname=="SC1"){
          parameter[i].push_back(8.199);
          parameter[i].push_back(-0.0261569);
          parameter[i].push_back(6.79677);
          parameter[i].push_back(-0.343845);
          parameter[i].push_back(-5.03578);
          parameter[i].push_back(2.7076);
          parameter[i].push_back(-0.420714);
        } else error("Atom name not known");
      } else if(Rname=="GLN") {
        if(Aname=="BB") {
          parameter[i].push_back(10.728);
          parameter[i].push_back(-0.0391984);
          parameter[i].push_back(1.09264);
          parameter[i].push_back(-0.261555);
          parameter[i].push_back(-2.21245);
          parameter[i].push_back(1.62071);
          parameter[i].push_back(-0.322325);
        } else if(Aname=="SC1"){
          parameter[i].push_back(8.317);
          parameter[i].push_back(-0.229045);
          parameter[i].push_back(12.6338);
          parameter[i].push_back(-7.6719);
          parameter[i].push_back(-5.8376);
          parameter[i].push_back(5.53784);
          parameter[i].push_back(-1.12604);
        } else error("Atom name not known");
      } else if(Rname=="GLU") {
        if(Aname=="BB") {
          parameter[i].push_back(10.694);
          parameter[i].push_back(-0.0521961);
          parameter[i].push_back(1.11153);
          parameter[i].push_back(-0.491995);
          parameter[i].push_back(-1.86236);
          parameter[i].push_back(1.45332);
          parameter[i].push_back(-0.29708);
        } else if(Aname=="SC1"){
          parameter[i].push_back(8.544);
          parameter[i].push_back(-0.249555);
          parameter[i].push_back(12.8031);
          parameter[i].push_back(-8.42696);
          parameter[i].push_back(-4.66486);
          parameter[i].push_back(4.90004);
          parameter[i].push_back(-1.01204);
        } else error("Atom name not known");
      } else if(Rname=="GLY") {
        if(Aname=="BB") {
          parameter[i].push_back(9.977);
          parameter[i].push_back(-0.0285799);
          parameter[i].push_back(1.84236);
          parameter[i].push_back(-0.0315192);
          parameter[i].push_back(-2.88326);
          parameter[i].push_back(1.87323);
          parameter[i].push_back(-0.345773);
        } else error("Atom name not known");
      } else if(Rname=="HIS") {
        if(Aname=="BB") {
          parameter[i].push_back(10.721);
          parameter[i].push_back(-0.0379337);
          parameter[i].push_back(1.06028);
          parameter[i].push_back(-0.236143);
          parameter[i].push_back(-2.17819);
          parameter[i].push_back(1.58357);
          parameter[i].push_back(-0.31345);
        } else if(Aname=="SC1"){
          parameter[i].push_back(-0.424);
          parameter[i].push_back(0.665176);
          parameter[i].push_back(3.4369);
          parameter[i].push_back(2.93795);
          parameter[i].push_back(-5.18288);
          parameter[i].push_back(2.12381);
          parameter[i].push_back(-0.284224);
        } else if(Aname=="SC2"){
          parameter[i].push_back(5.363);
          parameter[i].push_back(-0.0176945);
          parameter[i].push_back(2.9506);
          parameter[i].push_back(-0.387018);
          parameter[i].push_back(-1.83951);
          parameter[i].push_back(0.9703);
          parameter[i].push_back(-0.1458);
        } else if(Aname=="SC3"){
          parameter[i].push_back(5.784);
          parameter[i].push_back(-0.0293129);
          parameter[i].push_back(2.74167);
          parameter[i].push_back(-0.520875);
          parameter[i].push_back(-1.62949);
          parameter[i].push_back(0.902379);
          parameter[i].push_back(-0.139957);
        } else error("Atom name not known");
      } else if(Rname=="ILE") {
        if(Aname=="BB") {
          parameter[i].push_back(10.699);
          parameter[i].push_back(-0.0188962);
          parameter[i].push_back(1.217);
          parameter[i].push_back(0.242481);
          parameter[i].push_back(-3.13898);
          parameter[i].push_back(2.07916);
          parameter[i].push_back(-0.392574);
        } else if(Aname=="SC1"){
          parameter[i].push_back(-4.448);
          parameter[i].push_back(1.20996);
          parameter[i].push_back(11.5141);
          parameter[i].push_back(6.98895);
          parameter[i].push_back(-19.1948);
          parameter[i].push_back(9.89207);
          parameter[i].push_back(-1.60877);
        } else error("Atom name not known");
      } else if(Rname=="LEU") {
        if(Aname=="BB") {
          parameter[i].push_back(10.692);
          parameter[i].push_back(-0.209448);
          parameter[i].push_back(1.73738);
          parameter[i].push_back(-1.33726);
          parameter[i].push_back(-1.3065);
          parameter[i].push_back(1.25273);
          parameter[i].push_back(-0.265001);
        } else if(Aname=="SC1"){
          parameter[i].push_back(-4.448);
          parameter[i].push_back(2.1063);
          parameter[i].push_back(6.72381);
          parameter[i].push_back(14.6954);
          parameter[i].push_back(-23.7197);
          parameter[i].push_back(10.7247);
          parameter[i].push_back(-1.59146);
        } else error("Atom name not known");
      } else if(Rname=="LYS") {
        if(Aname=="BB") {
          parameter[i].push_back(10.706);
          parameter[i].push_back(-0.0468629);
          parameter[i].push_back(1.09477);
          parameter[i].push_back(-0.432751);
          parameter[i].push_back(-1.94335);
          parameter[i].push_back(1.49109);
          parameter[i].push_back(-0.302589);
        } else if(Aname=="SC1"){
          parameter[i].push_back(-2.796);
          parameter[i].push_back(0.508044);
          parameter[i].push_back(7.91436);
          parameter[i].push_back(4.54097);
          parameter[i].push_back(-10.8051);
          parameter[i].push_back(4.96204);
          parameter[i].push_back(-0.724414);
        } else if(Aname=="SC2"){
          parameter[i].push_back(3.070);
          parameter[i].push_back(-0.0101448);
          parameter[i].push_back(4.67994);
          parameter[i].push_back(-0.792529);
          parameter[i].push_back(-2.09142);
          parameter[i].push_back(1.02933);
          parameter[i].push_back(-0.137787);
        } else error("Atom name not known");
      } else if(Rname=="MET") {
        if(Aname=="BB") {
          parameter[i].push_back(10.671);
          parameter[i].push_back(-0.0433724);
          parameter[i].push_back(1.13784);
          parameter[i].push_back(-0.40768);
          parameter[i].push_back(-2.00555);
          parameter[i].push_back(1.51673);
          parameter[i].push_back(-0.305547);
        } else if(Aname=="SC1"){
          parameter[i].push_back(5.85);
          parameter[i].push_back(-0.0485798);
          parameter[i].push_back(17.0391);
          parameter[i].push_back(-3.65327);
          parameter[i].push_back(-13.174);
          parameter[i].push_back(8.68286);
          parameter[i].push_back(-1.56095);
        } else error("Atom name not known");
      } else if(Rname=="PHE") {
        if(Aname=="BB") {
          parameter[i].push_back(10.741);
          parameter[i].push_back(-0.0317276);
          parameter[i].push_back(1.15599);
          parameter[i].push_back(0.0276186);
          parameter[i].push_back(-2.74757);
          parameter[i].push_back(1.88783);
          parameter[i].push_back(-0.363525);
        } else if(Aname=="SC1"){
          parameter[i].push_back(-0.636);
          parameter[i].push_back(0.527882);
          parameter[i].push_back(6.77612);
          parameter[i].push_back(3.18508);
          parameter[i].push_back(-8.92826);
          parameter[i].push_back(4.29752);
          parameter[i].push_back(-0.65187);
        } else if(Aname=="SC2"){
          parameter[i].push_back(-0.424);
          parameter[i].push_back(0.389174);
          parameter[i].push_back(4.11761);
          parameter[i].push_back(2.29527);
          parameter[i].push_back(-4.7652);
          parameter[i].push_back(1.97023);
          parameter[i].push_back(-0.262318);
        } else if(Aname=="SC3"){
          parameter[i].push_back(-0.424);
          parameter[i].push_back(0.38927);
          parameter[i].push_back(4.11708);
          parameter[i].push_back(2.29623);
          parameter[i].push_back(-4.76592);
          parameter[i].push_back(1.97055);
          parameter[i].push_back(-0.26238);
        } else error("Atom name not known");
      } else if(Rname=="PRO") {
        if(Aname=="BB") {
          parameter[i].push_back(11.434);
          parameter[i].push_back(-0.033323);
          parameter[i].push_back(0.472014);
          parameter[i].push_back(-0.290854);
          parameter[i].push_back(-1.81409);
          parameter[i].push_back(1.39751);
          parameter[i].push_back(-0.280407);
        } else if(Aname=="SC1"){
          parameter[i].push_back(-2.796);
          parameter[i].push_back(0.95668);
          parameter[i].push_back(6.84197);
          parameter[i].push_back(6.43774);
          parameter[i].push_back(-12.5068);
          parameter[i].push_back(5.64597);
          parameter[i].push_back(-0.825206);
        } else error("Atom name not known");
      } else if(Rname=="SER") {
        if(Aname=="BB") {
          parameter[i].push_back(10.699);
          parameter[i].push_back(-0.0325828);
          parameter[i].push_back(1.20329);
          parameter[i].push_back(-0.0674351);
          parameter[i].push_back(-2.60749);
          parameter[i].push_back(1.80318);
          parameter[i].push_back(-0.346803);
        } else if(Aname=="SC1"){
          parameter[i].push_back(3.298);
          parameter[i].push_back(-0.0366801);
          parameter[i].push_back(5.11077);
          parameter[i].push_back(-1.46774);
          parameter[i].push_back(-1.48421);
          parameter[i].push_back(0.800326);
          parameter[i].push_back(-0.108314);
        } else error("Atom name not known");
      } else if(Rname=="THR") {
        if(Aname=="BB") {
          parameter[i].push_back(10.697);
          parameter[i].push_back(-0.0242955);
          parameter[i].push_back(1.24671);
          parameter[i].push_back(0.146423);
          parameter[i].push_back(-2.97429);
          parameter[i].push_back(1.97513);
          parameter[i].push_back(-0.371479);
        } else if(Aname=="SC1"){
          parameter[i].push_back(2.366);
          parameter[i].push_back(0.0297604);
          parameter[i].push_back(11.9216);
          parameter[i].push_back(-9.32503);
          parameter[i].push_back(1.9396);
          parameter[i].push_back(0.0804861);
          parameter[i].push_back(-0.0302721);
        } else error("Atom name not known");
      } else if(Rname=="TRP") {
        if(Aname=="BB") {
          parameter[i].push_back(10.689);
          parameter[i].push_back(-0.0265879);
          parameter[i].push_back(1.17819);
          parameter[i].push_back(0.0386457);
          parameter[i].push_back(-2.75634);
          parameter[i].push_back(1.88065);
          parameter[i].push_back(-0.360217);
        } else if(Aname=="SC1"){
          parameter[i].push_back(0.084);
          parameter[i].push_back(0.752407);
          parameter[i].push_back(5.3802);
          parameter[i].push_back(4.09281);
          parameter[i].push_back(-9.28029);
          parameter[i].push_back(4.45923);
          parameter[i].push_back(-0.689008);
        } else if(Aname=="SC2"){
          parameter[i].push_back(5.739);
          parameter[i].push_back(0.0298492);
          parameter[i].push_back(4.60446);
          parameter[i].push_back(1.34463);
          parameter[i].push_back(-5.69968);
          parameter[i].push_back(2.84924);
          parameter[i].push_back(-0.433781);
        } else if(Aname=="SC3"){
          parameter[i].push_back(-0.424);
          parameter[i].push_back(0.388576);
          parameter[i].push_back(4.11859);
          parameter[i].push_back(2.29485);
          parameter[i].push_back(-4.76255);
          parameter[i].push_back(1.96849);
          parameter[i].push_back(-0.262015);
        } else if(Aname=="SC4"){
          parameter[i].push_back(-0.424);
          parameter[i].push_back(0.387685);
          parameter[i].push_back(4.12153);
          parameter[i].push_back(2.29144);
          parameter[i].push_back(-4.7589);
          parameter[i].push_back(1.96686);
          parameter[i].push_back(-0.261786);
        } else error("Atom name not known");
      } else if(Rname=="TYR") {
        if(Aname=="BB") {
          parameter[i].push_back(10.689);
          parameter[i].push_back(-0.0193526);
          parameter[i].push_back(1.18241);
          parameter[i].push_back(0.207318);
          parameter[i].push_back(-3.0041);
          parameter[i].push_back(1.99335);
          parameter[i].push_back(-0.376482);
        } else if(Aname=="SC1"){
          parameter[i].push_back(-0.636);
          parameter[i].push_back(0.528902);
          parameter[i].push_back(6.78168);
          parameter[i].push_back(3.17769);
          parameter[i].push_back(-8.93667);
          parameter[i].push_back(4.30692);
          parameter[i].push_back(-0.653993);
        } else if(Aname=="SC2"){
          parameter[i].push_back(-0.424);
          parameter[i].push_back(0.388811);
          parameter[i].push_back(4.11851);
          parameter[i].push_back(2.29545);
          parameter[i].push_back(-4.7668);
          parameter[i].push_back(1.97131);
          parameter[i].push_back(-0.262534);
        } else if(Aname=="SC3"){
          parameter[i].push_back(4.526);
          parameter[i].push_back(-0.00381305);
          parameter[i].push_back(5.8567);
          parameter[i].push_back(-0.214086);
          parameter[i].push_back(-4.63649);
          parameter[i].push_back(2.52869);
          parameter[i].push_back(-0.39894);
        } else error("Atom name not known");
      } else if(Rname=="VAL") {
        if(Aname=="BB") {
          parameter[i].push_back(10.691);
          parameter[i].push_back(-0.0162929);
          parameter[i].push_back(1.24446);
          parameter[i].push_back(0.307914);
          parameter[i].push_back(-3.27446);
          parameter[i].push_back(2.14788);
          parameter[i].push_back(-0.403259);
        } else if(Aname=="SC1"){
          parameter[i].push_back(-3.516);
          parameter[i].push_back(1.62307);
          parameter[i].push_back(5.43064);
          parameter[i].push_back(9.28809);
          parameter[i].push_back(-14.9927);
          parameter[i].push_back(6.6133);
          parameter[i].push_back(-0.964977);
        } else error("Atom name not known");
      } else error("Residue not known");
    }
  } else {
    error("MOLINFO DATA not found\n");
  }
}

void SAXS::calculate(){
  if(pbc) makeWhole();

  const unsigned size=getNumberOfAtoms();
  
  unsigned stride=comm.Get_size();
  unsigned rank=comm.Get_rank();
  if(serial){
    stride=1;
    rank=0;
  }

  vector<Vector> deriv(numq*size);
  vector<Tensor> deriv_box(numq);
  vector<double> sum(numq);

  #pragma omp parallel for num_threads(OpenMP::getNumThreads())
  for (unsigned k=0; k<numq; k++) {
    const unsigned kdx=k*size;
    for (unsigned i=rank; i<size-1; i+=stride) {
      const unsigned kdxi=kdx+i;
      const double FF=2.*FF_value[k][i];
      const Vector posi=getPosition(i);
      for (unsigned j=i+1; j<size ; j++) {
        const Vector c_distances = delta(posi,getPosition(j));
        const double m_distances = c_distances.modulo();
        const double qdist     = q_list[k]*m_distances;
        const double FFF = FF*FF_value[k][j];
        const double tsq = FFF*sin(qdist)/qdist;
        const double tcq = FFF*cos(qdist);
        const double tmp = (tcq-tsq)/(m_distances*m_distances);
        const Vector dd  = c_distances*tmp;
        sum[k] += tsq;
        deriv_box[k] += Tensor(c_distances,dd);
        deriv[kdxi ] -= dd;
        deriv[kdx+j] += dd;
      }
    }
  }

  if(!serial) {
    comm.Sum(&deriv[0][0], 3*deriv.size());
    comm.Sum(&deriv_box[0][0][0], numq*9);
    comm.Sum(&sum[0], numq);
  }

  #pragma omp parallel for num_threads(OpenMP::getNumThreads())
  for (unsigned k=0; k<numq; k++) {
    const unsigned kdx=k*size;
    Value* val=getPntrToComponent(k);
    for(unsigned i=0; i<size; i++) setAtomsDerivatives(val, i, deriv[kdx+i]);
    sum[k]+=FF_rank[k];
    setBoxDerivatives(val, -deriv_box[k]);
    val->set(sum[k]);
  }

}

}
}
