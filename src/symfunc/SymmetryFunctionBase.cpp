/* +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
   Copyright (c) 2015-2017 The plumed team
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
#include "SymmetryFunctionBase.h"
#include "core/PlumedMain.h"
#include "core/Atoms.h"
#include "multicolvar/MultiColvarBase.h"

namespace PLMD {
namespace symfunc {

void SymmetryFunctionBase::shortcutKeywords( Keywords& keys ) {
  keys.add("atoms-3","SPECIES","this keyword is used for colvars such as coordination number. In that context it specifies that plumed should calculate "
           "one coordination number for each of the atoms specified.  Each of these coordination numbers specifies how many of the "
           "other specified atoms are within a certain cutoff of the central atom.  You can specify the atoms here as another multicolvar "
           "action or using a MultiColvarFilter or ActionVolume action.  When you do so the quantity is calculated for those atoms specified "
           "in the previous multicolvar.  This is useful if you would like to calculate the Steinhardt parameter for those atoms that have a "
           "coordination number more than four for example");
  keys.add("atoms-4","SPECIESA","this keyword is used for colvars such as the coordination number.  In that context it species that plumed should calculate "
           "one coordination number for each of the atoms specified in SPECIESA.  Each of these cooordination numbers specifies how many "
           "of the atoms specifies using SPECIESB is within the specified cutoff.  As with the species keyword the input can also be specified "
           "using the label of another multicolvar");
  keys.add("atoms-4","SPECIESB","this keyword is used for colvars such as the coordination number.  It must appear with SPECIESA.  For a full explanation see "
           "the documentation for that keyword");
  keys.add("compulsory","NN","6","The n parameter of the switching function ");
  keys.add("compulsory","MM","0","The m parameter of the switching function; 0 implies 2*NN");
  keys.add("compulsory","D_0","0.0","The d_0 parameter of the switching function");
  keys.add("compulsory","R_0","The r_0 parameter of the switching function");
  keys.add("optional","SWITCH","the switching function that it used in the construction of the contact matrix");
  multicolvar::MultiColvarBase::shortcutKeywords( keys );
}

void SymmetryFunctionBase::expandMatrix( const bool& components, const std::string& lab, const std::vector<std::string>& words,
    const std::map<std::string,std::string>& keys,
    std::vector<std::vector<std::string> >& actions ) {
  if( !keys.count("SPECIES") && !keys.count("SPECIESA") ) return;

  std::vector<std::string> matinp; matinp.push_back( lab + "_mat:" ); matinp.push_back("CONTACT_MATRIX");
  if( keys.count("SPECIES") ) {
    matinp.push_back("GROUP=" + keys.find("SPECIES")->second );
  } else if( keys.count("SPECIESA") ) {
    matinp.push_back("GROUPA=" + keys.find("SPECIESA")->second ); matinp.push_back("GROUPB=" + keys.find("SPECIESB")->second );
  }
  if( keys.count("SWITCH") ) {
    matinp.push_back("SWITCH=" + keys.find("SWITCH")->second );
  } else if( keys.count("R_0") ) {
    matinp.push_back("R_0=" + keys.find("R_0")->second );
    matinp.push_back("D_0=" + keys.find("D_0")->second );
    matinp.push_back("NN=" + keys.find("NN")->second );
    matinp.push_back("MM=" + keys.find("MM")->second );
  } else {
    plumed_merror("could not interpret switching function definition");
  }
  if( components ) matinp.push_back("COMPONENTS");
  actions.push_back( matinp );
}

void SymmetryFunctionBase::registerKeywords( Keywords& keys ) {
  Action::registerKeywords( keys );
  ActionWithValue::registerKeywords( keys );
  ActionWithArguments::registerKeywords( keys );
  keys.add("compulsory","WEIGHT","");
  keys.add("numbered","VECTORS","");
  keys.addFlag("ONESHOT",false,"This forces all the elements of the row of the matrix to be computed prior to computing the symmetry function.  "
               "It should only be ever need to be used for testing.");
  keys.addFlag("USECOLS",false,"When this flag is present the CVs are calculated by summing over the columns rather than the rows.  You are thus calculating "
               "symmetry functions for the atoms in GROUPB rather than symmetry functions for the atoms in GROUPA.  The derivatives are much "
               "more expensive when this approach is used");
}

SymmetryFunctionBase::SymmetryFunctionBase(const ActionOptions&ao):
  Action(ao),
  ActionWithValue(ao),
  ActionWithArguments(ao),
  done_with_matrix_comput(true),
  usecols(false)
{
  if( keywords.exists("USECOLS") ) {
    parseFlag("USECOLS",usecols);
    if( usecols ) log.printf("  calculating symmetry functions for second group \n");
  }
  std::vector<std::string> alabels(1); std::vector<Value*> wval; parseArgumentList("WEIGHT",wval);
  if( wval.size()!=1 ) error("keyword WEIGHT should be provided with the label of a single action");
  alabels[0]=(wval[0]->getPntrToAction())->getLabel(); (wval[0]->getPntrToAction())->addActionToChain( alabels, this );
  log.printf("  using bond weights from matrix labelled %s \n",wval[0]->getName().c_str() );
  nderivatives=(wval[0]->getPntrToAction())->getNumberOfDerivatives();

  if( keywords.exists("VECTORS") ) {
    for(unsigned i=1; i<=3; ++i) {
      std::vector<Value*> vecs; parseArgumentList("VECTORS",i,vecs);
      if( vecs.size()!=1 ) error("keywords VECTORS should be provided with the label of a single action");
      if( wval[0]->getRank()!=vecs[0]->getRank() ) error("rank of weights does not match rank of vector");
      if( wval[0]->getRank()==2 ) {
        if( wval[0]->getShape()[0]!=vecs[0]->getShape()[0] || wval[0]->getShape()[1]!=vecs[0]->getShape()[1] ) {
          error("mismatched shapes of matrices in input");
        }
      } else if( wval[0]->getRank()==1 && wval[0]->getShape()[0]!=vecs[0]->getShape()[0] ) error("mismatched shapes of vectors in input");
      if( (wval[0]->getPntrToAction())->getLabel()!=(vecs[0]->getPntrToAction())->getLabel() ) {
        error("found mismatched vectors and weights in input to symmetry function - current not available, please email plumed list");
      }
      if( ((wval[0]->getPntrToAction())->getActionThatCalculates())->getLabel()!=((vecs[0]->getPntrToAction())->getActionThatCalculates())->getLabel() ) {
        error("found mismatched vectors and weights in input to symmetry function (2nd version) - current not available, please email plumed list");
      }
      alabels[0]=(vecs[0]->getPntrToAction())->getLabel(); (vecs[0]->getPntrToAction())->addActionToChain( alabels, this ); wval.push_back(vecs[0]);
      std::string dir="x"; if( i==2 ) dir="y"; else dir="z";
      log.printf("  %s direction of bond read from matrix labelled %s \n",dir.c_str(),vecs[0]->getName().c_str() );
    }
  }
  if( keywords.exists("ONESHOT") ) {
    bool oneshot; parseFlag("ONESHOT",oneshot);
    if( oneshot ) {
      done_with_matrix_comput=false;
      log.printf("  computing full matrix rows before computing symmetry function \n");
    }
  } else {
    done_with_matrix_comput=false;
  }
  // If we are doing the calculation as we compute matrix elements we must store all matrix elements
  // in rows.  Actually we store whole matrix because I don't want to make more complicated options
  if( !done_with_matrix_comput ) {
    // The -mat here is added to prevent this behaving like a proper stored value when updating forces
    for(unsigned i=0; i<wval.size(); ++i) wval[i]->buildDataStore( getLabel() + "-mat" );
  }
  requestArguments(wval,true); forcesToApply.resize( nderivatives );
  if( getPntrToArgument(0)->getRank()==2 ) {
    for(unsigned i=0; i<getPntrToArgument(0)->getShape()[0]; ++i) addTaskToList(i);
  }
  if( !usecols && plumed.getAtoms().getAllGroups().count(wval[0]->getPntrToAction()->getLabel()) ) {
    const auto m=plumed.getAtoms().getAllGroups().find(wval[0]->getPntrToAction()->getLabel());
    plumed.getAtoms().insertGroup( getLabel(), m->second );
  }
}

void SymmetryFunctionBase::addValueWithDerivatives() {
  std::vector<unsigned> shape;
  if( getPntrToArgument(0)->getRank()==2 ) {
    shape.resize(1);
    if( usecols ) shape[0]=getPntrToArgument(0)->getShape()[1];
    else shape[0]=getPntrToArgument(0)->getShape()[0];
    if( shape[0]==1 ) shape.resize(0);
  }
  if( shape.size()==0 ) ActionWithValue::addValueWithDerivatives( shape );
  else ActionWithValue::addValue( shape );
  setNotPeriodic();
  if( usecols ) getPntrToOutput( getNumberOfComponents()-1 )->buildColumnSums();
}

void SymmetryFunctionBase::addComponentWithDerivatives( const std::string& name ) {
  std::vector<unsigned> shape;
  if( getPntrToArgument(0)->getRank()==2 ) {
    shape.resize(1);
    if( usecols ) shape[0]=getPntrToArgument(0)->getShape()[1];
    else shape[0]=getPntrToArgument(0)->getShape()[0];
    if( shape[0]==1 ) shape.resize(0);
  }
  if( shape.size()==0 ) ActionWithValue::addComponentWithDerivatives(name,shape);
  else ActionWithValue::addComponent(name,shape);
  componentIsNotPeriodic(name);
  if( usecols ) getPntrToOutput( getNumberOfComponents()-1 )->buildColumnSums();
}

void SymmetryFunctionBase::buildCurrentTaskList( std::vector<unsigned>& tflags ) {
  plumed_assert( actionInChain() ); tflags.assign(tflags.size(),1);
}

void SymmetryFunctionBase::performTask( const unsigned& current, MultiValue& myvals ) const {
  if( !myvals.inVectorCall() && done_with_matrix_comput && !myvals.inMatrixRerun() ) {
    double weight = myvals.get( getPntrToArgument(0)->getPositionInStream() );
    if( fabs(weight)>epsilon ) {
      Vector dir; dir.zero();
      if( getNumberOfArguments()==4 ) {
        dir[0] = myvals.get( getPntrToArgument(1)->getPositionInStream() );
        dir[1] = myvals.get( getPntrToArgument(2)->getPositionInStream() );
        dir[2] = myvals.get( getPntrToArgument(3)->getPositionInStream() );
      }
      compute( weight, dir, myvals );
    }
  } else if( myvals.inVectorCall() ) {
    if( !done_with_matrix_comput ) {
      // Make sure tempory derivative space is set up if required
      if( !doNotCalculateDerivatives() ) {
        std::vector<double>& tmp_w( myvals.getSymfuncTemporyDerivatives( getPntrToArgument(0)->getPositionInStream() ) );
        if( tmp_w.size()<getNumberOfComponents()*getPntrToArgument(0)->getShape()[1] ) tmp_w.resize( getNumberOfComponents()*getPntrToArgument(0)->getShape()[1], 0 );
        std::vector<double>& tmp_x( myvals.getSymfuncTemporyDerivatives( getPntrToArgument(1)->getPositionInStream() ) );
        if( tmp_x.size()<getNumberOfComponents()*getPntrToArgument(1)->getShape()[1] ) tmp_x.resize( getNumberOfComponents()*getPntrToArgument(1)->getShape()[1], 0 );
        std::vector<double>& tmp_y( myvals.getSymfuncTemporyDerivatives( getPntrToArgument(2)->getPositionInStream() ) );
        if( tmp_y.size()<getNumberOfComponents()*getPntrToArgument(2)->getShape()[1] ) tmp_y.resize( getNumberOfComponents()*getPntrToArgument(2)->getShape()[1], 0 );
        std::vector<double>& tmp_z( myvals.getSymfuncTemporyDerivatives( getPntrToArgument(3)->getPositionInStream() ) );
        if( tmp_z.size()<getNumberOfComponents()*getPntrToArgument(3)->getShape()[1] ) tmp_z.resize( getNumberOfComponents()*getPntrToArgument(3)->getShape()[1], 0 );
      }
      computeSymmetryFunction( current, myvals );
      // And now the derivatives
      if( !doNotCalculateDerivatives() ) {
        ActionWithValue* av = (getPntrToArgument(0)->getPntrToAction())->getActionThatCalculates();
        unsigned aindex_start = myvals.getNumberOfIndicesInFirstBlock();
        unsigned matind = getPntrToArgument(0)->getPositionInMatrixStash();
        unsigned my_w = getPntrToArgument(0)->getPositionInStream();
        std::vector<double>& tmp_w( myvals.getSymfuncTemporyDerivatives(my_w) );
        // Turn off matrix element storing during rerun of calculations
        myvals.setMatrixStashForRerun();
        if( getNumberOfArguments()==4 ) {
          unsigned my_x = getPntrToArgument(1)->getPositionInStream();
          std::vector<double>& tmp_x( myvals.getSymfuncTemporyDerivatives(my_x) );
          unsigned my_y = getPntrToArgument(2)->getPositionInStream();
          std::vector<double>& tmp_y( myvals.getSymfuncTemporyDerivatives(my_y) );
          unsigned my_z = getPntrToArgument(3)->getPositionInStream();
          std::vector<double>& tmp_z( myvals.getSymfuncTemporyDerivatives(my_z) );
          for(unsigned j=0; j<myvals.getNumberOfStashedMatrixElements(matind); ++j) {
            unsigned wstart=0, jind = myvals.getStashedMatrixIndex(matind,j);
            // Check for derivatives and skip recalculation if there are none
            double totder=0;
            for(unsigned i=0; i<getNumberOfComponents(); ++i) {
              totder +=tmp_w[wstart+jind] + tmp_x[wstart+jind] + tmp_y[wstart+jind] + tmp_z[wstart+jind];
              wstart++;
            }
            if( fabs(totder)<epsilon ) continue ;
            // Rerun the task required
            wstart = 0; av->runTask( av->getLabel(), myvals.getTaskIndex(), current, aindex_start + jind, myvals );
            // Now add on the derivatives
            for(unsigned i=0; i<getNumberOfComponents(); ++i) {
              unsigned ostrn = getPntrToOutput(i)->getPositionInStream();
              for(unsigned k=0; k<myvals.getNumberActive(my_w); ++k) {
                unsigned kind=myvals.getActiveIndex(my_w,k);
                myvals.addDerivative( ostrn, arg_deriv_starts[i] + kind, tmp_w[wstart+jind]*myvals.getDerivative( my_w, kind ) );
              }
              for(unsigned k=0; k<myvals.getNumberActive(my_x); ++k) {
                unsigned kind=myvals.getActiveIndex(my_x,k);
                myvals.addDerivative( ostrn, arg_deriv_starts[i] + kind, tmp_x[wstart+jind]*myvals.getDerivative( my_x, kind ) );
              }
              for(unsigned k=0; k<myvals.getNumberActive(my_y); ++k) {
                unsigned kind=myvals.getActiveIndex(my_y,k);
                myvals.addDerivative( ostrn, arg_deriv_starts[i] + kind, tmp_y[wstart+jind]*myvals.getDerivative( my_y, kind ) );
              }
              for(unsigned k=0; k<myvals.getNumberActive(my_z); ++k) {
                unsigned kind=myvals.getActiveIndex(my_z,k);
                myvals.addDerivative( ostrn, arg_deriv_starts[i] + kind, tmp_z[wstart+jind]*myvals.getDerivative( my_z, kind ) );
              }
              tmp_w[wstart+jind]=tmp_x[wstart+jind]=tmp_y[wstart+jind]=tmp_z[wstart+jind]=0;
              wstart += getPntrToArgument(0)->getShape()[1];
            }
            // Clear the matrix elements for this task
            av->clearMatrixElements( myvals );
          }
        } else {
          for(unsigned j=0; j<myvals.getNumberOfStashedMatrixElements(matind); ++j) {
            unsigned jind = myvals.getStashedMatrixIndex(matind,j);
            // Rerun the task required
            av->runTask( av->getLabel(), myvals.getTaskIndex(), current, aindex_start + jind, myvals );
            // Now add on the derivatives
            for(unsigned i=0; i<getNumberOfComponents(); ++i) {
              unsigned wstart=0, ostrn = getPntrToOutput(i)->getPositionInStream();
              for(unsigned k=0; k<myvals.getNumberActive(my_w); ++k) {
                unsigned kind=myvals.getActiveIndex(my_w,k);
                myvals.addDerivative( ostrn, arg_deriv_starts[i] + kind, tmp_w[wstart+jind]*myvals.getDerivative( my_w, kind ) );
              }
              tmp_w[wstart+jind]=0; wstart += getPntrToArgument(0)->getShape()[1];
            }
            // Clear the matrix elements for this task
            av->clearMatrixElements( myvals );
          }
        }
        // Set the myvals object to store matrix elements
        myvals.setMatrixStashForNormalRun();
      }
    }
    // Update derivatives for indices
    if( !doNotCalculateDerivatives() ) updateDerivativeIndices( myvals );
  }
}

void SymmetryFunctionBase::updateDerivativeIndices( MultiValue& myvals ) const {
  unsigned istrn = getPntrToArgument(0)->getPositionInMatrixStash();
  std::vector<unsigned>& mat_indices( myvals.getMatrixIndices( istrn ) );
  for(unsigned i=0; i<myvals.getNumberOfMatrixIndices(istrn); ++i) {
    for(unsigned j=0; j<getNumberOfComponents(); ++j) {
      unsigned ostrn = getPntrToOutput(j)->getPositionInStream();
      myvals.updateIndex( ostrn, mat_indices[i] );
    }
  }
}

void SymmetryFunctionBase::computeSymmetryFunction( const unsigned& current, MultiValue& myvals ) const {
  Vector dir;
  unsigned matind = getPntrToArgument(0)->getPositionInMatrixStash();
  if( getNumberOfArguments()>1 ) {
    unsigned matind_x = getPntrToArgument(1)->getPositionInMatrixStash();
    unsigned matind_y = getPntrToArgument(2)->getPositionInMatrixStash();
    unsigned matind_z = getPntrToArgument(3)->getPositionInMatrixStash();
    for(unsigned j=0; j<myvals.getNumberOfStashedMatrixElements(matind); ++j) {
      unsigned jind = myvals.getStashedMatrixIndex(matind,j);
      double weight = myvals.getStashedMatrixElement( matind, jind );
      dir[0] = myvals.getStashedMatrixElement( matind_x, jind );
      dir[1] = myvals.getStashedMatrixElement( matind_y, jind );
      dir[2] = myvals.getStashedMatrixElement( matind_z, jind );
      myvals.setSymfuncTemporyIndex(jind); compute( weight, dir, myvals );
    }
  } else {
    for(unsigned j=0; j<myvals.getNumberOfStashedMatrixElements(matind); ++j) {
      unsigned jind = myvals.getStashedMatrixIndex(matind,j);
      double weight = myvals.getStashedMatrixElement( matind, jind );
      myvals.setSymfuncTemporyIndex(jind); compute( weight, dir, myvals );
    }
  }
}

void SymmetryFunctionBase::apply() {
  if( doNotCalculateDerivatives() ) return;
  if( forcesToApply.size()!=getNumberOfDerivatives() ) forcesToApply.resize( getNumberOfDerivatives() );
  std::fill(forcesToApply.begin(),forcesToApply.end(),0); unsigned mm=0;
  if( getForcesFromValues( forcesToApply ) ) { setForcesOnArguments( forcesToApply, mm ); }
}

}
}

