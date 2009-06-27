// $Id$
// Author: John Wu <John.Wu at acm.org>
//      Lawrence Berkeley National Laboratory
// Copyright 1998-2009 the Regents of the University of California
//
// implement the functions defined in qExpr.h
//
#include "util.h"
#include "part.h"
#include "qExpr.h"

#ifdef sun
#include <ieeefp.h>	// finite
#endif
#include <math.h>	// finite
#include <stdlib.h>
#include <limits.h>
#if defined(_WIN32)
#include <float.h>	// DBL_MAX, _finite
#endif
#if defined(unix) && !defined(DBL_MAX)
#include <values.h>
#endif

#include <set>		// std::set
#include <iterator>	// std::ostream_iterator
#include <algorithm>	// std::copy, std::sort

// the names of the operators used in ibis::compRange
const char* ibis::math::operator_name[] =
    {"?", "|", "&", "+", "-", "*", "/", "%", "-", "**"};
const char* ibis::math::stdfun1_name[] =
    {"acos", "asin", "atan", "ceil", "cos", "cosh", "exp", "fabs",
     "floor", "frexp", "log10", "log", "modf", "sin", "sinh", "sqrt",
     "tan", "tanh"};
const char* ibis::math::stdfun2_name[] = {"atan2", "fmod", "ldexp", "pow"};
bool ibis::math::preserveInputExpressions = false;

/// Operations performed include converting compRanges into qRanges,
/// qDiscreteRange into qContinuousRange, perform constant evaluations,
/// combining pairs of inverse functions.  This is necessary because the
/// parser always generates compRange instead of qRange.  The goal of
/// simplifying arithmetic expressions is to reduce the number of accesses
/// to the variable values (potentially reducing the number of disk
/// accesses).
///
/// @note Be aware that rearranging the arithmetic expressions may affect
/// the round-off perperties of these expressions, and therefore affect
/// their computed results.  Even though the typical differences might be
/// small (after ten significant digits), however, the differences could
/// accumulated and became noticeable.  To turn off this optimization, set
/// ibis::math::preserveInputExpressions to true.
void ibis::qExpr::simplify(ibis::qExpr*& expr) {
    if (expr == 0) return;
    LOGGER(ibis::gVerbose > 4)
	<< "ibis::qExpr::simplify --  input expression " << *expr;

    switch (expr->getType()) {
    default:
	break;
    case ibis::qExpr::LOGICAL_NOT:
	simplify(expr->left);
	break;
    case ibis::qExpr::LOGICAL_AND: {
	// TODO: add code to combine simple experessions on the same variable.
	simplify(expr->left);
	simplify(expr->right);
	bool emptyleft = (expr->left == 0 ||
			  ((expr->left->getType() == RANGE ||
			    expr->left->getType() == DRANGE) &&
			   reinterpret_cast<qRange*>(expr->left)->empty()));
	bool emptyright = (expr->right == 0 ||
			   ((expr->right->getType() == RANGE ||
			     expr->right->getType() == DRANGE) &&
			    reinterpret_cast<qRange*>(expr->right)->empty()));
	if (emptyleft) {
	    ibis::qExpr* tmp = expr->left;
	    expr->left = 0;
	    delete expr;
	    expr = tmp;
	}
	else if (emptyright) {
	    ibis::qExpr *tmp = expr->right;
	    expr->right = 0;
	    delete expr;
	    expr = tmp;
	}
	if (expr->left != 0 && expr->right != 0 &&
	    expr->left->type == ibis::qExpr::RANGE &&
	    expr->right->type == ibis::qExpr::RANGE &&
	    stricmp(static_cast<ibis::qRange*>(expr->left)->colName(),
		    static_cast<ibis::qRange*>(expr->right)->colName()) == 0) {
	    ibis::qContinuousRange* tm1 =
		static_cast<ibis::qContinuousRange*>(expr->left);
	    ibis::qContinuousRange* tm2 =
		static_cast<ibis::qContinuousRange*>(expr->right);
	    if ((tm1->left_op == ibis::qExpr::OP_LE ||
		 tm1->left_op == ibis::qExpr::OP_LT) &&
		(tm2->left_op == ibis::qExpr::OP_LE ||
		 tm2->left_op == ibis::qExpr::OP_LT) &&
		(tm1->right_op == ibis::qExpr::OP_LE ||
		 tm1->right_op == ibis::qExpr::OP_LT) &&
		(tm2->right_op == ibis::qExpr::OP_LE ||
		 tm2->right_op == ibis::qExpr::OP_LT)) { // two two-sided ranges
		if (tm1->lower < tm2->lower) {
		    tm1->left_op = tm2->left_op;
		    tm1->lower = tm2->lower;
		}
		else if (tm1->lower == tm2->lower &&
			 tm1->left_op == ibis::qExpr::OP_LE &&
			 tm2->left_op == ibis::qExpr::OP_LT) {
		    tm1->left_op = ibis::qExpr::OP_LT;
		}
		if (tm1->upper > tm2->upper) {
		    tm1->right_op = tm2->right_op;
		    tm1->upper = tm2->upper;
		}
		else if (tm1->upper == tm2->upper &&
			 tm1->right_op == ibis::qExpr::OP_LE &&
			 tm2->right_op == ibis::qExpr::OP_LT) {
		    tm1->right_op = ibis::qExpr::OP_LT;
		}
		expr->left = 0;
		delete expr;
		expr = tm1;
	    }
	    else if ((tm1->left_op == ibis::qExpr::OP_LE ||
		      tm1->left_op == ibis::qExpr::OP_LT) &&
		     (tm2->left_op == ibis::qExpr::OP_LE ||
		      tm2->left_op == ibis::qExpr::OP_LT) &&
		     (tm1->right_op == ibis::qExpr::OP_LE ||
		      tm1->right_op == ibis::qExpr::OP_LT) &&
		     (tm2->right_op == ibis::qExpr::OP_UNDEFINED)) {
		// tm1 two-sided range, tm2 one-sided
		if (tm1->lower < tm2->lower) {
		    tm1->left_op = tm2->left_op;
		    tm1->lower = tm2->lower;
		}
		else if (tm1->lower == tm2->lower &&
			 tm1->left_op == ibis::qExpr::OP_LE &&
			 tm2->left_op == ibis::qExpr::OP_LT) {
		    tm1->left_op = ibis::qExpr::OP_LT;
		}
		expr->left = 0;
		delete expr;
		expr = tm1;
	    }
	    else if ((tm1->left_op == ibis::qExpr::OP_LE ||
		      tm1->left_op == ibis::qExpr::OP_LT) &&
		     (tm2->left_op == ibis::qExpr::OP_LE ||
		      tm2->left_op == ibis::qExpr::OP_LT) &&
		     (tm2->right_op == ibis::qExpr::OP_LE ||
		      tm2->right_op == ibis::qExpr::OP_LT) &&
		     (tm1->right_op == ibis::qExpr::OP_UNDEFINED)) {
		// tm1 one-sided range, tm2 two-sided
		if (tm2->lower < tm1->lower) {
		    tm2->left_op = tm1->left_op;
		    tm2->lower = tm1->lower;
		}
		else if (tm1->lower == tm2->lower &&
			 tm2->left_op == ibis::qExpr::OP_LE &&
			 tm1->left_op == ibis::qExpr::OP_LT) {
		    tm2->left_op = ibis::qExpr::OP_LT;
		}
		expr->right = 0;
		delete expr;
		expr = tm2;
	    }
	    else if ((tm1->left_op == ibis::qExpr::OP_LE ||
		      tm1->left_op == ibis::qExpr::OP_LT) &&
		     (tm2->right_op == ibis::qExpr::OP_LE ||
		      tm2->right_op == ibis::qExpr::OP_LT) &&
		     (tm1->right_op == ibis::qExpr::OP_LE ||
		      tm1->right_op == ibis::qExpr::OP_LT) &&
		     (tm2->left_op == ibis::qExpr::OP_UNDEFINED)) {
		// tm1 two-sided range, tm2 one-sided
		if (tm1->upper > tm2->upper) {
		    tm1->right_op = tm2->right_op;
		    tm1->upper = tm2->upper;
		}
		else if (tm1->upper == tm2->upper &&
			 tm1->right_op == ibis::qExpr::OP_LE &&
			 tm2->right_op == ibis::qExpr::OP_LT) {
		    tm1->right_op = ibis::qExpr::OP_LT;
		}
		expr->left = 0;
		delete expr;
		expr = tm1;
	    }
	    else if ((tm1->right_op == ibis::qExpr::OP_LE ||
		      tm1->right_op == ibis::qExpr::OP_LT) &&
		     (tm2->left_op == ibis::qExpr::OP_LE ||
		      tm2->left_op == ibis::qExpr::OP_LT) &&
		     (tm2->right_op == ibis::qExpr::OP_LE ||
		      tm2->right_op == ibis::qExpr::OP_LT) &&
		     (tm1->left_op == ibis::qExpr::OP_UNDEFINED)) {
		// tm1 one-sided range, tm2 two-sided
		if (tm2->upper > tm1->upper) {
		    tm2->right_op = tm1->right_op;
		    tm2->upper = tm1->upper;
		}
		else if (tm1->upper == tm2->upper &&
			 tm2->right_op == ibis::qExpr::OP_LE &&
			 tm1->right_op == ibis::qExpr::OP_LT) {
		    tm2->right_op = ibis::qExpr::OP_LT;
		}
		expr->right = 0;
		delete expr;
		expr = tm2;
	    }
	    else if ((tm1->left_op == ibis::qExpr::OP_LE ||
		      tm1->left_op == ibis::qExpr::OP_LT) &&
		     (tm2->left_op == ibis::qExpr::OP_LE ||
		      tm2->left_op == ibis::qExpr::OP_LT) &&
		     (tm1->right_op == ibis::qExpr::OP_UNDEFINED) &&
		     (tm2->right_op == ibis::qExpr::OP_UNDEFINED)) {
		// both one-sided
		if (tm1->lower < tm2->lower) {
		    tm1->left_op = tm2->left_op;
		    tm1->lower = tm2->lower;
		}
		else if (tm1->lower == tm2->lower &&
			 tm1->left_op == ibis::qExpr::OP_LE &&
			 tm2->left_op == ibis::qExpr::OP_LT) {
		    tm1->left_op = ibis::qExpr::OP_LT;
		}
		expr->left = 0;
		delete expr;
		expr = tm1;
	    }
	    else if ((tm1->right_op == ibis::qExpr::OP_LE ||
		      tm1->right_op == ibis::qExpr::OP_LT) &&
		     (tm2->right_op == ibis::qExpr::OP_LE ||
		      tm2->right_op == ibis::qExpr::OP_LT) &&
		     (tm2->left_op == ibis::qExpr::OP_UNDEFINED) &&
		     (tm1->left_op == ibis::qExpr::OP_UNDEFINED)) {
		// both one-sided
		if (tm2->upper > tm1->upper) {
		    tm2->right_op = tm1->right_op;
		    tm2->upper = tm1->upper;
		}
		else if (tm2->upper == tm1->upper &&
			 tm1->right_op == ibis::qExpr::OP_LT &&
			 tm2->right_op == ibis::qExpr::OP_LE) {
		    tm2->right_op = tm1->right_op;
		}
		expr->right = 0;
		delete expr;
		expr = tm2;
	    }
	    else if ((tm1->left_op == ibis::qExpr::OP_LE ||
		      tm1->left_op == ibis::qExpr::OP_LT) &&
		     (tm2->right_op == ibis::qExpr::OP_LE ||
		      tm2->right_op == ibis::qExpr::OP_LT) &&
		     (tm1->right_op == ibis::qExpr::OP_UNDEFINED) &&
		     (tm2->left_op == ibis::qExpr::OP_UNDEFINED)) {
		// both one-sided
		tm1->right_op = tm2->right_op;
		tm1->upper = tm2->upper;
		expr->left = 0;
		delete expr;
		expr = tm1;
	    }
	    else if ((tm1->right_op == ibis::qExpr::OP_LE ||
		      tm1->right_op == ibis::qExpr::OP_LT) &&
		     (tm2->left_op == ibis::qExpr::OP_LE ||
		      tm2->left_op == ibis::qExpr::OP_LT) &&
		     (tm1->left_op == ibis::qExpr::OP_UNDEFINED) &&
		     (tm2->right_op == ibis::qExpr::OP_UNDEFINED)) {
		// both one-sided
		tm1->left_op = tm2->left_op;
		tm1->lower = tm2->lower;
		expr->left = 0;
		delete expr;
		expr = tm1;
	    }
	    else if ((tm1->left_op == ibis::qExpr::OP_LE ||
		      tm1->left_op == ibis::qExpr::OP_LT) &&
		     (tm1->right_op == ibis::qExpr::OP_LE ||
		      tm1->right_op == ibis::qExpr::OP_LT)) {
		if (tm2->left_op == ibis::qExpr::OP_EQ) {
		    if (tm1->inRange(tm2->lower)) {
			expr->right = 0;
			delete expr;
			expr = tm2;
		    }
		    else {
			delete expr;
			expr = 0;
		    }
		}
		else if (tm2->right_op == ibis::qExpr::OP_EQ) {
		    if (tm1->inRange(tm2->upper)) {
			expr->right = 0;
			delete expr;
			expr = tm2;
		    }
		    else {
			delete expr;
			expr = 0;
		    }
		}
	    }
	    else if ((tm2->left_op == ibis::qExpr::OP_LE ||
		      tm2->left_op == ibis::qExpr::OP_LT) &&
		     (tm2->right_op == ibis::qExpr::OP_LE ||
		      tm2->right_op == ibis::qExpr::OP_LT)) {
		if (tm1->left_op == ibis::qExpr::OP_EQ) {
		    if (tm2->inRange(tm1->lower)) {
			expr->left = 0;
			delete expr;
			expr = tm1;
		    }
		    else {
			delete expr;
			expr = 0;
		    }
		}
		else if (tm1->right_op == ibis::qExpr::OP_EQ) {
		    if (tm2->inRange(tm1->upper)) {
			expr->left = 0;
			delete expr;
			expr = tm1;
		    }
		    else {
			delete expr;
			expr = 0;
		    }
		}
	    }
	    else if ((tm1->left_op == ibis::qExpr::OP_LE ||
		      tm1->left_op == ibis::qExpr::OP_LT) &&
		     (tm1->right_op == ibis::qExpr::OP_UNDEFINED)) {
		if (tm2->left_op == ibis::qExpr::OP_EQ) {
		    if (tm1->inRange(tm2->lower)) {
			expr->right = 0;
			delete expr;
			expr = tm2;
		    }
		    else {
			delete expr;
			expr = 0;
		    }
		}
		else if (tm2->right_op == ibis::qExpr::OP_EQ) {
		    if (tm1->inRange(tm2->upper)) {
			expr->right = 0;
			delete expr;
			expr = tm2;
		    }
		    else {
			delete expr;
			expr = 0;
		    }
		}
	    }
	    else if ((tm2->left_op == ibis::qExpr::OP_UNDEFINED) &&
		     (tm2->right_op == ibis::qExpr::OP_LE ||
		      tm2->right_op == ibis::qExpr::OP_LT)) {
		if (tm1->left_op == ibis::qExpr::OP_EQ) {
		    if (tm2->inRange(tm1->lower)) {
			expr->left = 0;
			delete expr;
			expr = tm1;
		    }
		    else {
			delete expr;
			expr = 0;
		    }
		}
		else if (tm1->right_op == ibis::qExpr::OP_EQ) {
		    if (tm2->inRange(tm1->upper)) {
			expr->left = 0;
			delete expr;
			expr = tm1;
		    }
		    else {
			delete expr;
			expr = 0;
		    }
		}
	    }
	}
	break;}
    case ibis::qExpr::LOGICAL_OR:
    case ibis::qExpr::LOGICAL_XOR: {
	simplify(expr->left);
	simplify(expr->right);
	bool emptyleft = (expr->left == 0 ||
			  ((expr->left->getType() == RANGE ||
			    expr->left->getType() == DRANGE) &&
			   reinterpret_cast<qRange*>(expr->left)->empty()));
	bool emptyright = (expr->right == 0 ||
			   ((expr->right->getType() == RANGE ||
			     expr->right->getType() == DRANGE) &&
			    reinterpret_cast<qRange*>(expr->right)->empty()));
	if (emptyleft) {
	    if (emptyright) { // keep left
		ibis::qExpr* tmp = expr->left;
		expr->left = 0;
		delete expr;
		expr = tmp;
	    }
	    else { // keep right
		ibis::qExpr* tmp = expr->right;
		expr->right = 0;
		delete expr;
		expr = tmp;
	    }
	}
	else if (emptyright) { // keep left
	    ibis::qExpr *tmp = expr->left;
	    expr->left = 0;
	    delete expr;
	    expr = tmp;
	}
	break;}
    case ibis::qExpr::LOGICAL_MINUS: {
	simplify(expr->left);
	simplify(expr->right);
	bool emptyleft = (expr->left == 0 ||
			  ((expr->left->getType() == RANGE ||
			    expr->left->getType() == DRANGE) &&
			   reinterpret_cast<qRange*>(expr->left)->empty()));
	bool emptyright = (expr->right == 0 ||
			   ((expr->right->getType() == RANGE ||
			     expr->right->getType() == DRANGE) &&
			    reinterpret_cast<qRange*>(expr->right)->empty()));
	if (emptyleft || emptyright) {
	    // keep left: if left is empty, the overall result is empty;
	    // if the right is empty, the overall result is the left
	    ibis::qExpr* tmp = expr->left;
	    expr->left = 0;
	    delete expr;
	    expr = tmp;
	}
	break;}
    case ibis::qExpr::COMPRANGE: {
	ibis::compRange* cr = reinterpret_cast<ibis::compRange*>(expr);
	ibis::math::term *t1, *t2;
	t1 = reinterpret_cast<ibis::math::term*>(cr->getLeft());
	if (t1 != 0 && ibis::math::preserveInputExpressions == false) {
	    t2 = t1->reduce();
	    if (t2 != t1)
		cr->setLeft(t2);
	}

	t1 = reinterpret_cast<ibis::math::term*>(cr->getRight());
	if (t1 != 0 && ibis::math::preserveInputExpressions == false) {
	    t2 = t1->reduce();
	    if (t2 != t1)
		cr->setRight(t2);
	}

	t1 = reinterpret_cast<ibis::math::term*>(cr->getTerm3());
	if (t1 != 0 && ibis::math::preserveInputExpressions == false) {
	    t2 = t1->reduce();
	    if (t2 != t1)
		cr->setTerm3(t2);
	}

	if (cr->getLeft() != 0 && cr->getRight() != 0 && cr->getTerm3() != 0) {
	    ibis::math::term* tm1 =
		reinterpret_cast<ibis::math::term*>(cr->getLeft());
	    ibis::math::term* tm2 =
		reinterpret_cast<ibis::math::term*>(cr->getRight());
	    ibis::math::term* tm3 =
		reinterpret_cast<ibis::math::term*>(cr->getTerm3());
	    if (tm1->termType() == ibis::math::NUMBER &&
		tm3->termType() == ibis::math::NUMBER &&
		tm2->termType() == ibis::math::OPERATOR) {
		if (reinterpret_cast<ibis::math::term*>
		    (tm2->getLeft())->termType() == ibis::math::NUMBER &&
		    reinterpret_cast<ibis::math::term*>
		    (tm2->getRight())->termType() ==
		    ibis::math::VARIABLE) {
		    const ibis::math::bediener& op2 =
			*static_cast<ibis::math::bediener*>(tm2);
		    double cnst = static_cast<ibis::math::number*>
			(tm2->getLeft())->eval();
		    switch (op2.operador) {
		    default: break; // do nothing
		    case ibis::math::PLUS: {
			ibis::qContinuousRange *tmp = new
			    ibis::qContinuousRange
			    (tm1->eval()-cnst, cr->leftOperator(),
			     static_cast<const ibis::math::variable*>
			     (op2.getRight())->variableName(),
			     cr->rightOperator(), tm2->eval()-cnst);
			delete expr;
			expr = tmp;
			cr = 0;
			break;}
		    case ibis::math::MINUS: {
			ibis::qContinuousRange *tmp = new
			    ibis::qContinuousRange
			    (tm1->eval()+cnst, cr->leftOperator(),
			     static_cast<const ibis::math::variable*>
			     (op2.getRight())->variableName(),
			     cr->rightOperator(), tm2->eval()+cnst);
			delete expr;
			expr = tmp;
			cr = 0;
			break;}
		    case ibis::math::MULTIPLY: {
			if (cnst > 0.0) {
			    ibis::qContinuousRange *tmp = new
				ibis::qContinuousRange
				(tm1->eval()/cnst, cr->leftOperator(),
				 static_cast<const ibis::math::variable*>
				 (op2.getRight())->variableName(),
				 cr->rightOperator(), tm2->eval()/cnst);
			    delete expr;
			    expr = tmp;
			    cr = 0;
			}
			break;}
		    }
		}
	    }
	} // three terms
	else if (cr->getLeft() != 0 && cr->getRight() != 0) { // two terms
	    ibis::math::term* tm1 =
		reinterpret_cast<ibis::math::term*>(cr->getLeft());
	    ibis::math::term* tm2 =
		reinterpret_cast<ibis::math::term*>(cr->getRight());
	    if (tm1->termType() == ibis::math::NUMBER &&
		tm2->termType() == ibis::math::OPERATOR) {
		ibis::math::number* nm1 =
		    static_cast<ibis::math::number*>(tm1);
		ibis::math::bediener* op2 =
		    static_cast<ibis::math::bediener*>(tm2);
		ibis::math::term* tm21 =
		    reinterpret_cast<ibis::math::term*>(tm2->getLeft());
		ibis::math::term* tm22 =
		    reinterpret_cast<ibis::math::term*>(tm2->getRight());
		if (tm21->termType() == ibis::math::NUMBER) {
		    switch (op2->operador) {
		    default: break;
		    case ibis::math::PLUS: {
			nm1->val -= tm21->eval();
			cr->getRight() = tm22;
			op2->getRight() = 0;
			delete op2;
			cr = 0;
			ibis::qExpr::simplify(expr);
			break;}
		    case ibis::math::MINUS: {
			cr->getLeft() = tm22;
			nm1->val = tm21->eval() - nm1->val;
			cr->getRight() = nm1;
			op2->getRight() = 0;
			delete op2;
			cr = 0;
			ibis::qExpr::simplify(expr);
			break;}
		    case ibis::math::MULTIPLY: {
			const double cnst = tm21->eval();
			if (cnst > 0.0) {
			    nm1->val /= cnst;
			    cr->getRight() = tm22;
			    op2->getRight() = 0;
			    delete op2;
			    cr = 0;
			    ibis::qExpr::simplify(expr);
			}
			else {
			    nm1->val /= tm21->eval();
			    op2->getRight() = 0;
			    delete op2;
			    cr->getRight() = nm1;
			    cr->getLeft() = tm22;
			    cr = 0;
			    ibis::qExpr::simplify(expr);
			}
			break;}
		    case ibis::math::DIVIDE: {
			nm1->val = tm21->eval() / nm1->val;
			cr->getLeft() = tm22;
			cr->getRight() = nm1;
			op2->getRight() = 0;
			delete op2;
			cr = 0;
			ibis::qExpr::simplify(expr);
			break;}
		    }
		}
		else if (tm22->termType() == ibis::math::NUMBER) {
		    switch (op2->operador) {
		    default: break;
		    case ibis::math::PLUS: {
			nm1->val -= tm21->eval();
			cr->getRight() = tm22;
			op2->getLeft() = 0;
			delete op2;
			cr = 0;
			ibis::qExpr::simplify(expr);
			break;}
		    case ibis::math::MINUS: {
			nm1->val += tm22->eval();
			cr->getRight() = tm21;
			op2->getLeft() = 0;
			delete op2;
			cr = 0;
			ibis::qExpr::simplify(expr);
			break;}
		    case ibis::math::MULTIPLY: {
			const double cnst = tm22->eval();
			if (cnst > 0.0) {
			    cr->getRight() = tm21;
			    nm1->val /= cnst;
			    op2->getLeft() = 0;
			    delete op2;
			    cr = 0;
			    ibis::qExpr::simplify(expr);
			}
			else {
			    nm1->val /= tm22->eval();
			    op2->getLeft() = 0;
			    delete op2;
			    cr->getRight() = nm1;
			    cr->getLeft() = tm21;
			    cr = 0;
			    ibis::qExpr::simplify(expr);
			}
			break;}
		    case ibis::math::DIVIDE: {
			nm1->val *= tm22->eval();
			cr->getRight() = tm21;
			op2->getLeft() = 0;
			delete op2;
			cr = 0;
			ibis::qExpr::simplify(expr);
			break;}
		    }
		}
	    }
	    else if (tm1->termType() == ibis::math::OPERATOR &&
		     tm2->termType() == ibis::math::NUMBER) {
		ibis::math::bediener* op1 =
		    static_cast<ibis::math::bediener*>(tm1);
		ibis::math::number* nm2 =
		    static_cast<ibis::math::number*>(tm2);
		ibis::math::term* tm11 =
		    reinterpret_cast<ibis::math::term*>(tm1->getLeft());
		ibis::math::term* tm12 =
		    reinterpret_cast<ibis::math::term*>(tm1->getRight());
		if (tm11->termType() == ibis::math::NUMBER) {
		    switch (op1->operador) {
		    default: break;
		    case ibis::math::PLUS: {
			nm2->val -= tm11->eval();
			cr->getLeft() = tm12;
			op1->getRight() = 0;
			delete op1;
			cr = 0;
			ibis::qExpr::simplify(expr);
			break;}
		    case ibis::math::MINUS: {
			cr->getRight() = tm12;
			nm2->val = tm11->eval() - nm2->val;
			cr->getLeft() = nm2;
			op1->getRight() = 0;
			cr = 0;
			delete op1;
			ibis::qExpr::simplify(expr);
			break;}
		    case ibis::math::MULTIPLY: {
			const double cnst = tm11->eval();
			if (cnst > 0.0) {
			    cr->getLeft() = tm12;
			    nm2->val /= cnst;
			    op1->getRight() = 0;
			    delete op1;
			    cr = 0;
			    ibis::qExpr::simplify(expr);
			}
			else {
			    nm2->val /= tm11->eval();
			    op1->getRight() = 0;
			    delete op1;
			    cr->getLeft() = nm2;
			    cr->getRight() = tm12;
			    cr = 0;
			    ibis::qExpr::simplify(expr);
			}
			break;}
		    case ibis::math::DIVIDE: {
			if (nm2->val > 0.0) {
			    nm2->val = tm11->eval() / nm2->val;
			    cr->getLeft() = nm2;
			    cr->getRight() = tm12;
			    op1->getRight() = 0;
			    delete op1;
			    cr = 0;
			    ibis::qExpr::simplify(expr);
			}
			break;}
		    }
		}
	    }
	}

	if (cr != 0 && cr != expr) {
#ifdef DEBUG
	    LOGGER(ibis::gVerbose >= 0)
		<< "replace a compRange with a qRange " << *expr;
#endif
	    expr = cr->simpleRange();
	    delete cr;
	}
	else if (expr->getType() == ibis::qExpr::COMPRANGE &&
		 static_cast<ibis::compRange*>(expr)->isSimpleRange()) {
#ifdef DEBUG
	    LOGGER(ibis::gVerbose >= 0)
		<< "replace a compRange with a qRange " << *expr;
#endif
	    cr = static_cast<ibis::compRange*>(expr);
	    expr = cr->simpleRange();
	    delete cr;
	}
	break;}
    case ibis::qExpr::RANGE: { // a continuous range
// 	ibis::qContinuousRange *cr =
// 	    reinterpret_cast<ibis::qContinuousRange*>(expr);
// 	if (cr->empty()) {
// 	    expr = 0;
// 	    delete cr;
// 	}
	break;}
    case ibis::qExpr::DRANGE: { // break a DRANGE into multiple RANGE
// 	ibis::qDiscreteRange *dr =
// 	    reinterpret_cast<ibis::qDiscreteRange*>(expr);
// 	ibis::qExpr *tmp = dr->convert();
// 	delete expr;
// 	expr = tmp;
	break;}
    case ibis::qExpr::MSTRING: { // break a MSTRING into multiple STRING
	ibis::qExpr *tmp = reinterpret_cast<ibis::qMultiString*>(expr)
	    ->convert();
	delete expr;
	expr = tmp;
	break;}
    case ibis::qExpr::JOIN: {
	ibis::math::term *range =
	    reinterpret_cast<ibis::rangeJoin*>(expr)->getRange();
	if (range != 0 && ibis::math::preserveInputExpressions == false) {
	    ibis::math::term *tmp = range->reduce();
	    if (tmp != range)
		reinterpret_cast<ibis::rangeJoin*>(expr)->setRange(tmp);
	}
	break;}
    } // switch(...

    if (ibis::gVerbose > 4) {
	ibis::util::logger lg;
	lg.buffer() << "ibis::qExpr::simplify -- output expression ";
	if (expr)
	    lg.buffer() << *expr;
	else
	    lg.buffer() << "is nil";
    }
} // ibis::qExpr::simplify

void ibis::qExpr::print(std::ostream& out) const {
    switch (type) {
    case LOGICAL_AND: {
	out << '(';
	left->print(out);
	out << " AND ";
	right->print(out);
	out << ')';
	break;
    }
    case LOGICAL_OR: {
	out << '(';
	left->print(out);
	out << " OR ";
	right->print(out);
	out << ')';
	break;
    }
    case LOGICAL_XOR: {
	out << '(';
	left->print(out);
	out << " XOR ";
	right->print(out);
	out << ')';
	break;
    }
    case LOGICAL_MINUS: {
	out << '(';
	left->print(out);
	out << " ANDNOT ";
	right->print(out);
	out << ')';
	break;
    }
    case LOGICAL_NOT: {
	out << "( ! " << *left << ')';
	break;
    }
    default:
	LOGGER(ibis::gVerbose >= 0) << "UNKNOWN LOGICAL OPERATOR";
    } // end of outer-mode switch statement
} // ibis::qExpr::print

// make the expression tree lean left
void ibis::qExpr::adjust() {
    ibis::qExpr* lptr = left;
    ibis::qExpr* rptr = right;
    if (left && right) {
	if (type == LOGICAL_AND || type == LOGICAL_OR || type == LOGICAL_XOR) {
	    if (type == right->type) {
		if (type == left->type) {
		    right = rptr->left;
		    rptr->left = left;
		    left = rptr;
		}
		else if (left->left == 0 && left->right == 0) {
		    right = lptr;
		    left = rptr;
		}
	    }
	    else if (left->isTerminal() && ! right->isTerminal()) {
		right = lptr;
		left = rptr;
	    }
	}
    }
    if (left && !(left->isTerminal()))
	left->adjust();
    if (right && !(right->isTerminal()))
	right->adjust();
} // ibis::qExpr::adjust

// reorder the expression so that the lightest weight is one the left side of
// a group of commutable operators
double ibis::qExpr::reorder(const ibis::qExpr::weight& wt) {
    double ret = 0.0;
    if (directEval()) {
	ret = wt(this);
	return ret;
    }

    if (ibis::gVerbose > 6) {
	ibis::util::logger lg;
	lg.buffer() << "ibis::qExpr::reorder -- input: ";
	print(lg.buffer());
    }

    adjust(); // to make sure the evaluation tree is a chain
    std::vector<ibis::qExpr*> terms;
    std::vector<double> wgt;
    ibis::qExpr* ptr;
    if (type == LOGICAL_AND || type == LOGICAL_OR || type == LOGICAL_XOR) {
	uint32_t i, j, k;
	double tmp;
	if (right->directEval()) {
	    ret = wt(right);
	} // if (right->directEval())
	else {
	    ret = right->reorder(wt);
	}
	terms.push_back(right);
	wgt.push_back(ret);

	ptr = left;
	while (ptr->type == type) {
	    // loop for left child of the same type
	    if (ptr->right->directEval()) {
		tmp = wt(ptr->right);
	    }
	    else {
		tmp = ptr->right->reorder(wt);
	    }
	    terms.push_back(ptr->right);
	    wgt.push_back(tmp);
	    ptr = ptr->left;
	    ret += tmp;
	}

	// left child is no longer the same type
	if (ptr->directEval()) {
	    tmp = wt(ptr);
	}
	else {
	    tmp = ptr->reorder(wt);
	}
	terms.push_back(ptr);
	wgt.push_back(tmp);
	ret += tmp;

	// all node connected by the same operator are collected together in
	// terms.  Next, separate the terminal nodes from the others
	i = 0;
	j = terms.size() - 1;
	while (i < j) {
	    if (terms[i]->directEval()) {
		++ i;
	    }
	    else if (terms[j]->directEval()) {
		ptr = terms[i];
		terms[i] = terms[j];
		terms[j] = ptr;
		-- j;
		++ i;
	    }
	    else {
		-- j;
	    }
	}
	if (terms[i]->directEval())
	    ++ i;

	// sort the array terms[i,...] according to wgt -- the heaviest
	// elements are ordered first because they are copied first back
	// into the tree structure as the right nodes, when the tree is
	// travesed in-order(and left-to-right), this results in the
	// lightest elements being evaluated first
	k = terms.size() - 1; // terms.size() >= 2
	for (i = 0; i < k; ++i) {
	    j = i + 1;
	    // find the one with largest weight in [i+1, ...)
	    for (uint32_t i0 = i+2; i0 <= k; ++ i0) {
		if ((wgt[i0] > wgt[j]) ||
		    (wgt[i0] == wgt[j] && terms[i0]->directEval() &&
		     ! (terms[j]->directEval())))
		    j = i0;
	    }

	    if (wgt[i] < wgt[j] ||
		(wgt[i] == wgt[j] && terms[j]->directEval() &&
		 ! (terms[i]->directEval()))) {
		// term i is not the largest, or term i can not be directly
		// evaluated
		ptr = terms[i];
		terms[i] = terms[j];
		terms[j] = ptr;
		tmp = wgt[i];
		wgt[i] = wgt[j];
		wgt[j] = tmp;
	    }
	    else { // term i is the largest, term j must be second largest
		++ i;
		if (j > i) {
		    ptr = terms[i];
		    terms[i] = terms[j];
		    terms[j] = ptr;
		    tmp = wgt[i];
		    wgt[i] = wgt[j];
		    wgt[j] = tmp;
		}
	    }
	}

#ifdef DEBUG
	if (ibis::gVerbose > 0) {
	    ibis::util::logger lg(4);
	    lg.buffer() << "DEBUG: qExpr::reorder(" << *this
			<< ") -- (expression:weight,...)\n";
	    for (i = 0; i < terms.size(); ++ i)
		lg.buffer() << *(terms[i]) << ":" << wgt[i] << ", ";
	}
#endif

	// populate the tree -- copy the heaviest nodes first to the right
	ptr = this;
	for (i = 0; i < k; ++ i) {
	    ptr->right = terms[i];
	    if (i+1 < k)
		ptr = ptr->left;
	}
	ptr->left = terms[k];
    } // if (type == LOGICAL_AND...
    else if (type == LOGICAL_MINUS) {
	ret = left->reorder(wt);
	ret += right->reorder(wt);
    } // else if (type == LOGICAL_MINUS)

    if (ibis::gVerbose > 6) {
	ibis::util::logger lg;
	lg.buffer() << "ibis::qExpr::reorder -- output: ";
	print(lg.buffer());
    }
    return ret;
} // ibis::qExpr::reorder

/// It returns 0 if there is a mixture of simple and complex conditions.
/// In this case, both simple and tail would be non-nil.  The return value
/// is -1 if all conditions are complex and 1 if all conditions are simple.
/// In these two cases, both simple and tail are nil.
int ibis::qExpr::separateSimple(ibis::qExpr *&simple,
				ibis::qExpr *&tail) const {
    if (ibis::gVerbose > 12) {
	ibis::util::logger lg;
	lg.buffer() << "ibis::qExpr::separateSimple -- input: ";
	print(lg.buffer());
    }

    int ret = INT_MAX;
    std::vector<const ibis::qExpr*> terms;
    const ibis::qExpr* ptr;
    if (type == LOGICAL_AND) {
	uint32_t i, j;
	// after adjust only one term is on the right-hand side
	terms.push_back(right);

	ptr = left;
	while (ptr->type == type) {
	    // loop for left child of the same type
	    terms.push_back(ptr->right);
	    ptr = ptr->left;
	}

	// left child is no longer the same type
	terms.push_back(ptr);

	// all node connected by the same operator are collected together in
	// terms.  Next, separate the simple nodes from the others
	i = 0;
	j = terms.size() - 1;
	while (i < j) {
	    if (terms[i]->isSimple()) {
		++ i;
	    }
	    else if (terms[j]->isSimple()) {
		ptr = terms[i];
		terms[i] = terms[j];
		terms[j] = ptr;
		-- j;
		++ i;
	    }
	    else {
		-- j;
	    }
	}
	if (terms[i]->isSimple())
	    ++ i;

#ifdef DEBUG
	if (ibis::gVerbose > 0) {
	    ibis::util::logger lg(4);
	    lg.buffer() << "qExpr::separateSimple -- terms joined with AND\n";
	    for (i=0; i<terms.size(); ++i)
		lg.buffer() << *(terms[i]) << "\n";
	}
#endif

	if (i > 1 && i < terms.size()) {
	    // more than one term, need AND operators
	    simple = new ibis::qExpr(ibis::qExpr::LOGICAL_AND,
				     terms[0]->dup(), terms[1]->dup());
	    for (j = 2; j < i; ++ j)
		simple = new ibis::qExpr(ibis::qExpr::LOGICAL_AND,
					 simple, terms[j]->dup());
	}
	else if (i == 1) {
	    simple = terms[0]->dup();
	}
	else { // no simple conditions, or all simple conditions
	    simple = 0;
	}
	if (i == 0 || i >= terms.size()) {
	    // no complex conditions, or only complex conditions
	    tail = 0;
	}
	else if (terms.size() > i+1) { // more than one complex terms
	    tail = new ibis::qExpr(ibis::qExpr::LOGICAL_AND,
				   terms[i]->dup(), terms[i+1]->dup());
	    for (j = i+2; j < terms.size(); ++ j)
		tail = new ibis::qExpr(ibis::qExpr::LOGICAL_AND,
				       tail, terms[j]->dup());
	}
	else { // only one complex term
	    tail = terms[i]->dup();
	}
	if (i == 0) // nothing simple
	    ret = -1;
	else if (i < terms.size()) // mixed simple and complex
	    ret = 0;
	else // all simple
	    ret = 1;
    } // if (type == LOGICAL_AND)
    else if (isSimple()) {
	simple = 0;
	tail = 0;
	ret = 1;
    }
    else {
	simple = 0;
	tail = 0;
	ret = -1;
    }

    if (ibis::gVerbose > 12) {
	ibis::util::logger lg;
	switch (ret) {
	default:
	case 0:
	    if (simple) {
		lg.buffer() << "ibis::qExpr::separateSimple -- simple  "
		    "conditions: ";
		simple->print(lg.buffer());
		lg.buffer() << "\n";
	    }
	    if (tail) {
		lg.buffer()<< "ibis::qExpr::separateSimple -- complex "
		    "conditions: ";
		tail->print(lg.buffer());
		lg.buffer() << "\n";
	    }
	    break;
	case -1:
	    lg.buffer() << "ibis::qExpr::separateSimple -- no simple terms";
	    break;
	case 1:
	    lg.buffer() << "ibis::qExpr::separateSimple -- all simple terms";
	    break;
	}
    }
    return ret;
} // ibis::qExpr::separateSimple

ibis::qRange* ibis::qExpr::findRange(const char *vname) {
    ibis::qRange *ret = 0;
    if (type == RANGE || type == DRANGE) {
	ret = reinterpret_cast<ibis::qRange*>(this);
	if (stricmp(vname, ret->colName()) != 0)
	    ret = 0;
	return ret;
    }
    else if (type == LOGICAL_AND) {
	if (left)
	    ret = left->findRange(vname);
	if (ret == 0) {
	    if (right)
		ret = right->findRange(vname);
	}
	return ret;
    }
    else {
	return ret;
    }
} // ibis::qExpr::findRange

bool ibis::qExpr::hasJoin() const {
    if (type == JOIN) {
	return true;
    }
    else if (left) {
	if (right)
	    return left->hasJoin() || right->hasJoin();
	else
	    return left->hasJoin();
    }
    else if (right) {
	return right->hasJoin();
    }
    else {
	return false;
    }
} // ibis::qExpr::hasJoin

void ibis::qExpr::extractJoins(std::vector<const rangeJoin*>& terms)
    const {
    if (type == LOGICAL_AND) {
	if (left != 0)
	    left->extractJoins(terms);
	if (right != 0)
	    right->extractJoins(terms);
    }
    else if (type == JOIN) {
	terms.push_back(reinterpret_cast<const rangeJoin*>(this));
    }
} // ibis::qExpr::extractJoins

// construct a qRange directly from a string representation of the constants
ibis::qContinuousRange::qContinuousRange
(const char *lstr, qExpr::COMPARE lop, const char* prop,
 qExpr::COMPARE rop, const char *rstr)
    : qRange(RANGE), name(ibis::util::strnewdup(prop)),
      left_op(lop), right_op(rop) {
    // first convert the values from string format to double format
    if (lstr)
	lower = (*lstr)?atof(lstr):(-DBL_MAX);
    else
	lower = -DBL_MAX;
    if (rstr)
	upper = (*rstr)?atof(rstr):(DBL_MAX);
    else
	upper = DBL_MAX;
#ifdef DEBUG
    LOGGER(ibis::gVerbose >= 0)
	<< "column: " << name << "\n"
	<< "left string: \"" << (lstr?lstr:"<NULL>")
	<< "\", right string: \"" << (rstr?rstr:"<NULL>") << "\"\n"
	<< lower << ", " << name << ", " << upper;
#endif

    // make show the left operator is OP_LE and the right one is OP_LT
    if (left_op == qExpr::OP_LT) { // change open left boundary to close one
	left_op = qExpr::OP_LE;
	lower = ibis::util::incrDouble(lower);
    }
    else if (left_op == qExpr::OP_EQ) {
	right_op = qExpr::OP_UNDEFINED;
	upper = lower;
    }
    if (right_op == qExpr::OP_LE) { // change closed right boundary to open
	right_op = qExpr::OP_LT;
	upper = ibis::util::incrDouble(upper);
    }
    else if (right_op == qExpr::OP_EQ) {
	left_op = qExpr::OP_UNDEFINED;
	lower = upper;
    }
} // constructor of qContinuousRange

void ibis::qContinuousRange::restrictRange(double left, double right) {
    if ((left_op == OP_GT || left_op == OP_GE) &&
	(right_op == OP_GT || right_op == OP_GE)) { // swap left and right
	if (left_op == OP_GT)
	    left_op = OP_LT;
	else
	    left_op = OP_LE;
	if (right_op == OP_GT)
	    right_op = OP_LT;
	else
	    right_op = OP_LE;

	double tmp = lower;
	lower = upper;
	upper = tmp;
    }
    if (((left_op == OP_LT || left_op == OP_LE) && lower < left) ||
	(left_op == OP_UNDEFINED &&
	 (right_op == OP_LT || right_op == OP_LE))) {
	lower = left;
	left_op = OP_LE;
    }
   if (((right_op == OP_LT || right_op == OP_LE) && upper > right) ||
	((left_op == OP_LT || left_op == OP_LE) &&
	 right_op == OP_UNDEFINED)) {
	upper = right;
	right_op = OP_LE;
    }
    if ((left_op == OP_EQ && right_op == OP_UNDEFINED &&
	 (lower < left || lower > right)) ||
	(left_op == OP_UNDEFINED && right_op == OP_EQ &&
	 (upper < left || upper > right))) { // empty range
	left_op = OP_EQ;
	right_op = OP_EQ;
	lower = left;
	upper = (right > left ? right : left + 1.0);
    }
} // ibis::qContinuousRange::restrictRange

bool ibis::qContinuousRange::empty() const {
    if ((left_op == OP_LT || left_op == OP_LE) &&
	(right_op == OP_LT || right_op == OP_LE)) {
	return (lower > upper || (lower == upper &&
				  (left_op != OP_LE || right_op != OP_LE)));
    }
    else if (left_op == OP_EQ && right_op == OP_EQ) {
	return (lower != upper);
    }
    else if ((left_op == OP_GT || left_op == OP_GE) &&
	     (right_op == OP_GT || right_op == OP_GE)) {
	return (upper > lower ||
		(lower == upper && (left_op != OP_GE || right_op != OP_GE)));
    }
    else {
	return false;
    }
} // ibis::qContinuousRange::empty

void ibis::qContinuousRange::print(std::ostream& out) const {
    if (name == 0 || *name == 0 ||
	(left_op == OP_UNDEFINED && right_op == OP_UNDEFINED)) {
	out << "ILL-DEFINED-RANGE";
	return;
    }

    switch (left_op) {
    case OP_EQ: {
	out << lower << "==";
	break;
    }
    case OP_LT: {
	out << lower << '<';
	break;
    } // case OP_LT
    case OP_LE: {
	out << lower << "<=";
	break;
    } // case OP_LE
    case OP_GT: {
	out << lower << '>';
	break;
    } // case OP_GT
    case OP_GE: {
	out << lower << ">=";
	break;
    } // case OP_GE
    default:
	break;
    } // switch (left_op)
    out << name;
    switch (right_op) {
    case OP_EQ:
	out << "==" << upper;
	break;
    case OP_LT:
	out << '<' << upper;
	break;
    case OP_LE:
	out << "<=" << upper;
	break;
    case OP_GT:
	out << '>' << upper;
	break;
    case OP_GE:
	out << ">=" << upper;
	break;
    default:
	break;
    } // end of switch right_op
} // ibis::qContinuousRange::print

void ibis::qContinuousRange::printRange(std::ostream& out) const {
    if (name == 0) return;
    if (getLeft() != 0) getLeft()->printRange(out);
    if (getRight() != 0) getRight()->printRange(out);
    if (getLeft() == 0 && getRight() == 0 &&
	(left_op != OP_UNDEFINED || right_op != OP_UNDEFINED)) {
	out << name << "\t";
	if (left_op != OP_UNDEFINED) {
	    uint64_t lval = static_cast<uint64_t>(lower);
	    if (right_op != OP_UNDEFINED) {
		uint64_t uval = static_cast<uint64_t>(upper);
		out << (lval == lower ? lval : lower) << "\t"
		    << (uval == upper ? uval : upper) << std::endl;
	    }
	    else {
		out << (lval == lower ? lval : lower) << std::endl;
	    }
	}
	else { // right_op must not be OP_UNDEFINED
	    uint64_t ival = static_cast<uint64_t>(upper);
	    out << (ival == upper ? ival : upper) << std::endl;
	}
    }
} // ibis::qContinuousRange::printRange

// the constructor of qString.  The string rs must have matching quote if it
// is quoted.  It may also contain meta character '\' that is used to escape
// the quote and other characters.  The meta character will also be striped.
ibis::qString::qString(const char* ls, const char* rs) :
    qExpr(ibis::qExpr::STRING), lstr(ibis::util::strnewdup(ls)) {
    // need to remove leading and trailing quote and the meta characters
    rstr = new char[1+strlen(rs)];
    const char quote = ('"' == *rs || '\'' == *rs) ? *rs : 0;
    const char* cptr = rs;
    char* dptr = rstr;
    if (quote) ++cptr;
    while (*cptr != quote) {
	if (*cptr != '\\') {
	    *dptr = *cptr;
	}
	else {
	    ++cptr;
	    *dptr = *cptr;
	}
	++cptr; ++dptr;
    }
    *dptr = 0; // terminate rstr with the NULL character
}

void ibis::qString::print(std::ostream& out) const {
    if (lstr && rstr)
	out << '(' << lstr << " == \"" << rstr << "\")";
    else
	out << "NULL";
}

/// Record all variables in @c term recursively.
void
ibis::math::barrel::recordVariable(const ibis::math::term* const t) {
    if (t != 0) {
	if (t->termType() == ibis::math::VARIABLE) {
	    static_cast<const ibis::math::variable*>(t)
		->recordVariable(*this);
	}
	else {
	    if (t->getLeft() != 0)
		recordVariable(static_cast<const ibis::math::term*>
			       (t->getLeft()));
	    if (t->getRight() != 0)
		recordVariable(static_cast<const ibis::math::term*>
			       (t->getRight()));
	}
    }
} // ibis::math::barrel::recordVariable

/// Return true if the two @c barrels contain the same set of variables,
/// otherwise false.
bool
ibis::math::barrel::equivalent(const ibis::math::barrel& rhs) const {
    if (varmap.size() != rhs.varmap.size()) return false;

    bool ret = true;
    termMap::const_iterator ilhs = varmap.begin();
    termMap::const_iterator irhs = rhs.varmap.begin();
    while (ilhs != varmap.end() && ret) {
	ret = (0 == stricmp((*ilhs).first, (*irhs).first));
	++ ilhs;
	++ irhs;
    }
    return ret;
} // ibis::math::barrel::equivalent

// constructors of concrete terms in ibis::compRange
ibis::math::stdFunction1::stdFunction1(const char* name) {
    if (0 == stricmp(name, "ACOS"))
	ftype = ibis::math::ACOS;
    else if (0 == stricmp(name, "ASIN"))
	ftype = ibis::math::ASIN;
    else if (0 == stricmp(name, "ATAN"))
	ftype = ibis::math::ATAN;
    else if (0 == stricmp(name, "CEIL"))
	ftype = ibis::math::CEIL;
    else if (0 == stricmp(name, "COS"))
	ftype = ibis::math::COS;
    else if (0 == stricmp(name, "COSH"))
	ftype = ibis::math::COSH;
    else if (0 == stricmp(name, "EXP"))
	ftype = ibis::math::EXP;
    else if (0 == stricmp(name, "FABS") || 0 == stricmp(name, "ABS"))
	ftype = ibis::math::FABS;
    else if (0 == stricmp(name, "FLOOR"))
	ftype = ibis::math::FLOOR;
    else if (0 == stricmp(name, "FREXP"))
	ftype = ibis::math::FREXP;
    else if (0 == stricmp(name, "LOG10"))
	ftype = ibis::math::LOG10;
    else if (0 == stricmp(name, "LOG"))
	ftype = ibis::math::LOG;
    else if (0 == stricmp(name, "MODF"))
	ftype = ibis::math::MODF;
    else if (0 == stricmp(name, "SIN"))
	ftype = ibis::math::SIN;
    else if (0 == stricmp(name, "SINH"))
	ftype = ibis::math::SINH;
    else if (0 == stricmp(name, "SQRT"))
	ftype = ibis::math::SQRT;
    else if (0 == stricmp(name, "TAN"))
	ftype = ibis::math::TAN;
    else if (0 == stricmp(name, "TANH"))
	ftype = ibis::math::TANH;
    else {
	LOGGER(ibis::gVerbose >= 0)
	    << "ibis::math::stdFunction1::stdFunction1(" << name
	    << ") UNKNOWN (one-argument) function name";
	throw "unknown function name";
    }
} // constructor of ibis::math::stdFunction1

ibis::math::term* ibis::math::stdFunction1::reduce() {
#if defined(_DEBUG)
    LOGGER(ibis::gVerbose > 4)
	<< "DEBUG -- stdFunction1::reduce  input:  " << *this;
#endif
    ibis::math::term *lhs =
	static_cast<ibis::math::term*>(getLeft());
    if (lhs->termType() == ibis::math::OPERATOR ||
	lhs->termType() == ibis::math::STDFUNCTION1 ||
	lhs->termType() == ibis::math::STDFUNCTION2) {
	ibis::math::term *tmp = lhs->reduce();
	if (tmp != lhs) { // replace LHS with the new term
	    setLeft(tmp);
	    lhs = tmp;
	}
    }

    ibis::math::term *ret = this;
    if (lhs->termType() == ibis::math::NUMBER) {
	double arg = lhs->eval();
	switch (ftype) {
	case ACOS: ret = new ibis::math::number(acos(arg)); break;
	case ASIN: ret = new ibis::math::number(asin(arg)); break;
	case ATAN: ret = new ibis::math::number(atan(arg)); break;
	case CEIL: ret = new ibis::math::number(ceil(arg)); break;
	case COS: ret = new ibis::math::number(cos(arg)); break;
	case COSH: ret = new ibis::math::number(cosh(arg)); break;
	case EXP: ret = new ibis::math::number(exp(arg)); break;
	case FABS: ret = new ibis::math::number(fabs(arg)); break;
	case FLOOR: ret = new ibis::math::number(floor(arg)); break;
	case FREXP: {int expptr;
	ret = new ibis::math::number(frexp(arg, &expptr)); break;}
	case LOG10: ret = new ibis::math::number(log10(arg)); break;
	case LOG: ret = new ibis::math::number(log(arg)); break;
	case MODF: {double intptr;
	ret = new ibis::math::number(modf(arg, &intptr)); break;}
	case SIN: ret = new ibis::math::number(sin(arg)); break;
	case SINH: ret = new ibis::math::number(sinh(arg)); break;
	case SQRT: ret = new ibis::math::number(sqrt(arg)); break;
	case TAN: ret = new ibis::math::number(tan(arg)); break;
	case TANH: ret = new ibis::math::number(tanh(arg)); break;
	default: break;
	}
    }
    else if (ftype == ACOS && lhs->termType() ==
	     ibis::math::STDFUNCTION1) {
	ibis::math::stdFunction1 *tmp =
	    reinterpret_cast<ibis::math::stdFunction1*>(lhs);
	if (tmp->ftype == COS) {
	    ret = static_cast<ibis::math::term*>(tmp->getLeft());
	    tmp->getLeft() = 0;
	}
    }
    else if (ftype == COS && lhs->termType() ==
	     ibis::math::STDFUNCTION1) {
	ibis::math::stdFunction1 *tmp =
	    reinterpret_cast<ibis::math::stdFunction1*>(lhs);
	if (tmp->ftype == ACOS) {
	    ret = static_cast<ibis::math::term*>(tmp->getLeft());
	    tmp->getLeft() = 0;
	}
    }
    else if (ftype == ASIN && lhs->termType() ==
	     ibis::math::STDFUNCTION1) {
	ibis::math::stdFunction1 *tmp =
	    reinterpret_cast<ibis::math::stdFunction1*>(lhs);
	if (tmp->ftype == SIN) {
	    ret = static_cast<ibis::math::term*>(tmp->getLeft());
	    tmp->getLeft() = 0;
	}
    }
    else if (ftype == SIN && lhs->termType() ==
	     ibis::math::STDFUNCTION1) {
	ibis::math::stdFunction1 *tmp =
	    reinterpret_cast<ibis::math::stdFunction1*>(lhs);
	if (tmp->ftype == ASIN) {
	    ret = static_cast<ibis::math::term*>(tmp->getLeft());
	    tmp->getLeft() = 0;
	}
    }
    else if (ftype == ATAN && lhs->termType() ==
	     ibis::math::STDFUNCTION1) {
	ibis::math::stdFunction1 *tmp =
	    reinterpret_cast<ibis::math::stdFunction1*>(lhs);
	if (tmp->ftype == TAN) {
	    ret = static_cast<ibis::math::term*>(tmp->getLeft());
	    tmp->getLeft() = 0;
	}
    }
    else if (ftype == TAN && lhs->termType() ==
	     ibis::math::STDFUNCTION1) {
	ibis::math::stdFunction1 *tmp =
	    reinterpret_cast<ibis::math::stdFunction1*>(lhs);
	if (tmp->ftype == ATAN) {
	    ret = static_cast<ibis::math::term*>(tmp->getLeft());
	    tmp->getLeft() = 0;
	}
    }
    else if (ftype == EXP && lhs->termType() ==
	     ibis::math::STDFUNCTION1) {
	ibis::math::stdFunction1 *tmp =
	    reinterpret_cast<ibis::math::stdFunction1*>(lhs);
	if (tmp->ftype == LOG) {
	    ret = static_cast<ibis::math::term*>(tmp->getLeft());
	    tmp->getLeft() = 0;
	}
    }
    else if (ftype == LOG && lhs->termType() ==
	     ibis::math::STDFUNCTION1) {
	ibis::math::stdFunction1 *tmp =
	    reinterpret_cast<ibis::math::stdFunction1*>(lhs);
	if (tmp->ftype == EXP) {
	    ret = static_cast<ibis::math::term*>(tmp->getLeft());
	    tmp->getLeft() = 0;
	}
    }
#if defined(_DEBUG)
    LOGGER(ibis::gVerbose > 4)
	<< "DEBUG -- stdFunction1::reduce output:  " << *ret;
#endif
    return ret;
} // ibis::math::stdfunction1::reduce

ibis::math::stdFunction2::stdFunction2(const char* name) {
    if (0 == stricmp(name, "ATAN2"))
	ftype = ibis::math::ATAN2;
    else if (0 == stricmp(name, "FMOD"))
	ftype = ibis::math::FMOD;
    else if (0 == stricmp(name, "LDEXP"))
	ftype = ibis::math::LDEXP;
    else if (0 == stricmp(name, "POW") || 0 == stricmp(name, "POWER"))
	ftype = ibis::math::POW;
    else {
	LOGGER(ibis::gVerbose >= 0)
	    << "ibis::math::stdFunction2::stdFunction2(" << name
	    << ") UNKNOWN (two-argument) function name";
	throw "unknown function name";
    }
} // constructor of ibis::math::stdFunction2

void ibis::math::bediener::print(std::ostream& out) const {
    switch (operador) {
    case ibis::math::NEGATE:
	out << "(-";
	getRight()->print(out);
	out << ')';
	break;
    case ibis::math::UNKNOWN:
	out << "unknown operator ?";
	break;
    default:
	out << "(";
	getLeft()->print(out);
	out << " " << operator_name[operador] << " ";
	getRight()->print(out);
	out << ")";
    }
} // ibis::math::bediener::print

ibis::math::term* ibis::math::bediener::reduce() {
    reorder(); // reorder the expression for easier reduction

#if defined(_DEBUG)
    LOGGER(ibis::gVerbose > 4)
	<< "DEBUG -- bediener::reduce  input:  " << *this;
#endif
    ibis::math::term *lhs =
	reinterpret_cast<ibis::math::term*>(getLeft());
    ibis::math::term *rhs =
	reinterpret_cast<ibis::math::term*>(getRight());
    if (lhs != 0 && (lhs->termType() == ibis::math::OPERATOR ||
		     lhs->termType() == ibis::math::STDFUNCTION1 ||
		     lhs->termType() == ibis::math::STDFUNCTION2)) {
	ibis::math::term *tmp = lhs->reduce();
	if (tmp != lhs) { // replace LHS with the new term
	    setLeft(tmp);
	    lhs = tmp;
	}
    }
    if (rhs != 0 && (rhs->termType() == ibis::math::OPERATOR ||
		     rhs->termType() == ibis::math::STDFUNCTION1 ||
		     rhs->termType() == ibis::math::STDFUNCTION2)) {
	ibis::math::term *tmp = rhs->reduce();
	if (tmp != rhs) { // replace RHS with the new term
	    setRight(tmp);
	    rhs = tmp;
	}
    }
    if (lhs == 0 || rhs == 0) return this;

    ibis::math::term *ret = this;
    switch (operador) {
    default:
    case ibis::math::UNKNOWN:
	break;
    case ibis::math::NEGATE: {
	if (lhs->termType() == ibis::math::NUMBER)
	    ret = new ibis::math::number(- lhs->eval());
	break;}
    case ibis::math::BITOR: {
	if (lhs->termType() == ibis::math::NUMBER &&
	    rhs->termType() == ibis::math::NUMBER) {
	    uint64_t i1 = (uint64_t) lhs->eval();
	    uint64_t i2 = (uint64_t) rhs->eval();
	    ret = new ibis::math::number(static_cast<double>(i1 | i2));
	}
	break;}
    case ibis::math::BITAND: {
	if (lhs->termType() == ibis::math::NUMBER &&
	    rhs->termType() == ibis::math::NUMBER) {
	    uint64_t i1 = (uint64_t) lhs->eval();
	    uint64_t i2 = (uint64_t) rhs->eval();
	    ret = new ibis::math::number(static_cast<double>(i1 & i2));
	}
	break;}
    case ibis::math::PLUS: {
	if (lhs->termType() == ibis::math::NUMBER &&
	    rhs->termType() == ibis::math::NUMBER) {
	    // both sides are numbers
	    ret = new ibis::math::number(lhs->eval() + rhs->eval());
	}
	else if (lhs->termType() == ibis::math::NUMBER &&
		 lhs->eval() == 0.0) {
	    ret = static_cast<ibis::math::term*>(getRight());
	    getRight() = 0;
	}
	else if (rhs->termType() == ibis::math::NUMBER &&
		 rhs->eval() == 0.0) {
	    ret = static_cast<ibis::math::term*>(getLeft());
	    getLeft() = 0;
	}
	else if (lhs->termType() == ibis::math::VARIABLE &&
		 rhs->termType() == ibis::math::VARIABLE &&
		 strcmp(static_cast<ibis::math::variable*>
			(lhs)->variableName(),
			static_cast<ibis::math::variable*>
			(rhs)->variableName()) == 0) {
	    // both sides are the same variable name
	    number *ntmp = new number(2.0);
	    bediener *btmp = new bediener(MULTIPLY);
	    btmp->getLeft() = ntmp;
	    btmp->getRight() = getRight();
	    getRight() = 0;
	    ret = btmp;
	}
	else if (lhs->termType() == ibis::math::OPERATOR &&
		 rhs->termType() == ibis::math::OPERATOR &&
		 static_cast<ibis::math::term*>(lhs->getLeft())->termType()
		 == ibis::math::NUMBER &&
		 static_cast<ibis::math::term*>(rhs->getLeft())->termType()
		 == ibis::math::NUMBER &&
		 static_cast<ibis::math::term*>
		 (lhs->getRight())->termType() == ibis::math::VARIABLE &&
		 static_cast<ibis::math::term*>
		 (rhs->getRight())->termType() == ibis::math::VARIABLE &&
		 strcmp(static_cast<ibis::math::variable*>
			(lhs->getRight())->variableName(),
			static_cast<ibis::math::variable*>
			(rhs->getRight())->variableName()) == 0) {
	    ret = lhs->dup();
	    static_cast<ibis::math::number*>(ret->getLeft())->val +=
		static_cast<ibis::math::term*>(rhs->getLeft())->eval();
	}
	break;}
    case ibis::math::MINUS: {
	if (lhs->termType() == ibis::math::NUMBER &&
	    rhs->termType() == ibis::math::NUMBER) {
	    ret = new ibis::math::number(lhs->eval() - rhs->eval());
	}
	else if (rhs->termType() == ibis::math::NUMBER &&
		 rhs->eval() == 0.0) {
	    ret = static_cast<ibis::math::term*>(getLeft());
	    getLeft() = 0;
	}
	else if (lhs->termType() == ibis::math::VARIABLE &&
		 rhs->termType() == ibis::math::VARIABLE &&
		 strcmp(static_cast<ibis::math::variable*>
			(lhs)->variableName(),
			static_cast<ibis::math::variable*>
			(rhs)->variableName()) == 0) {
	    // both sides are the same variable name
	    ret = new number(0.0);
	}
	else if (lhs->termType() == ibis::math::OPERATOR &&
		 rhs->termType() == ibis::math::OPERATOR &&
		 static_cast<ibis::math::term*>(lhs->getLeft())->termType()
		 == ibis::math::NUMBER &&
		 static_cast<ibis::math::term*>(rhs->getLeft())->termType()
		 == ibis::math::NUMBER &&
		 static_cast<ibis::math::term*>
		 (lhs->getRight())->termType() == ibis::math::VARIABLE &&
		 static_cast<ibis::math::term*>
		 (rhs->getRight())->termType() == ibis::math::VARIABLE &&
		 strcmp(static_cast<ibis::math::variable*>
			(lhs->getRight())->variableName(),
			static_cast<ibis::math::variable*>
			(rhs->getRight())->variableName()) == 0) {
	    ret = lhs->dup();
	    static_cast<ibis::math::number*>(ret->getLeft())->val -=
		static_cast<ibis::math::term*>(rhs->getLeft())->eval();
	}
	break;}
    case ibis::math::MULTIPLY: {
	if (lhs->termType() == ibis::math::NUMBER &&
	    lhs->eval() == 0.0) {
	    ret = new ibis::math::number(0.0);
	}
	else if (rhs->termType() == ibis::math::NUMBER &&
		 rhs->eval() == 0.0) {
	    ret = new ibis::math::number(0.0);
	}
	else if (lhs->termType() == ibis::math::NUMBER &&
		 rhs->termType() == ibis::math::NUMBER) {
	    ret = new ibis::math::number(lhs->eval() * rhs->eval());
	}
	else if (lhs->termType() == ibis::math::NUMBER &&
		 lhs->eval() == 1.0) { // multiply by one
	    ret = static_cast<ibis::math::term*>(getRight());
	    getRight() = 0;
	}
	else if (rhs->termType() == ibis::math::NUMBER &&
		 rhs->eval() == 1.0) { // multiply by one
	    ret = static_cast<ibis::math::term*>(getLeft());
	    getLeft() = 0;
	}
	else if (lhs->termType() == ibis::math::NUMBER &&
		 rhs->termType() == ibis::math::OPERATOR &&
		 static_cast<ibis::math::bediener*>
		 (rhs->getLeft())->operador == ibis::math::MULTIPLY &&
		 static_cast<ibis::math::term*>
		 (rhs->getLeft())->termType() == ibis::math::NUMBER) {
	    ret = static_cast<ibis::math::term*>(getRight());
	    static_cast<ibis::math::number*>(ret->getLeft())->val *=
		lhs->eval();
	    getRight() = 0;
	}
	else if (rhs->termType() == ibis::math::NUMBER &&
		 lhs->termType() == ibis::math::OPERATOR &&
		 static_cast<ibis::math::bediener*>
		 (lhs->getLeft())->operador == ibis::math::MULTIPLY &&
		 static_cast<ibis::math::term*>
		 (lhs->getLeft())->termType() == ibis::math::NUMBER) {
	    ret = static_cast<ibis::math::term*>(getLeft());
	    static_cast<ibis::math::number*>(ret->getLeft())->val *=
		rhs->eval();
	    getLeft() = 0;
	}
	break;}
    case ibis::math::DIVIDE: {
	if (lhs->termType() == ibis::math::NUMBER &&
	    lhs->eval() == 0.0) { // zero
	    ret = new ibis::math::number(0.0);
	}
	else if (rhs->termType() == ibis::math::NUMBER &&
		 (rhs->eval() < -DBL_MAX || rhs->eval() > DBL_MAX)) {
	    ret = new ibis::math::number(0.0);
	}
	else if (lhs->termType() == ibis::math::NUMBER &&
		 rhs->termType() == ibis::math::NUMBER) {
	    ret = new ibis::math::number(lhs->eval() / rhs->eval());
	}
	else if (rhs->termType() == ibis::math::NUMBER &&
		 lhs->termType() == ibis::math::OPERATOR &&
		 static_cast<ibis::math::bediener*>
		 (lhs->getLeft())->operador == ibis::math::MULTIPLY &&
		 static_cast<ibis::math::term*>
		 (lhs->getLeft())->termType() == ibis::math::NUMBER) {
	    ret = lhs->dup();
	    static_cast<ibis::math::number*>(ret->getLeft())->val /=
		rhs->eval();
	}
	break;}
    case ibis::math::POWER: {
	if (rhs->termType() == ibis::math::NUMBER &&
	    rhs->eval() == 0.0) { // zeroth power
	    ret = new ibis::math::number(1.0);
	}
	else if (lhs->termType() == ibis::math::NUMBER &&
		 lhs->eval() == 0.0) { // zero raise to some power
	    ret = new ibis::math::number(0.0);
	}
	else if (lhs->termType() == ibis::math::NUMBER &&
		 rhs->termType() == ibis::math::NUMBER) { // constant
	    ret = new ibis::math::number
		(pow(lhs->eval(), rhs->eval()));
	}
	break;}
    }

    if (ret != this) {
	ibis::math::term *tmp = ret->reduce();
	if (tmp != ret) {
	    delete ret;
	    ret = tmp;
	}
    }
#if defined(_DEBUG)
    LOGGER(ibis::gVerbose > 4)
	<< "DEBUG -- bediener::reduce output:  " << *ret;
#endif
    return ret;
} // ibis::math::bediener::reduce

// For operators whose two operands can be exchanged, this function makes
// sure the constants are move to the part that can be evaluated first.
void ibis::math::bediener::reorder() {
    // reduce the use of operator - and operator /
    convertConstants();
#if _DEBUG+0 > 1 || DEBUG+0 > 1
    LOGGER(ibis::gVerbose > 6)
	<< "DEBUG -- bediener::reorder  input:  " << *this;
#endif

    std::vector< ibis::math::term* > terms;
    if (operador == ibis::math::BITOR ||
	operador == ibis::math::BITAND ||
	operador == ibis::math::PLUS ||
	operador == ibis::math::MULTIPLY) {
	// first linearize -- put all terms to be rearranged in a list
	linearize(operador, terms);

	// make sure the numbers appears last in the list
	// there are at least two elements in terms
	uint32_t i = 0;
	uint32_t j = terms.size() - 1;
	while (i < j) {
	    if (terms[j]->termType() == ibis::math::NUMBER) {
		-- j;
	    }
	    else if (terms[i]->termType() == ibis::math::NUMBER) {
		ibis::math::term *ptr = terms[i];
		terms[i] = terms[j];
		terms[j] = ptr;
		-- j;
		++ i;
	    }
	    else {
		++ i;
	    }
	}

	// put the list of terms into a skewed tree
	ibis::math::term *ptr = this;
	j = terms.size() - 1;
	for (i = 0; i < j; ++ i) {
	    ptr->setRight(terms[i]);
	    if (i+1 < j) {
		if (reinterpret_cast<ibis::math::term*>
		    (ptr->getLeft())->termType() !=
		    ibis::math::OPERATOR ||
		    reinterpret_cast<ibis::math::bediener*>
		    (ptr->getLeft())->operador != operador)
		    ptr->setLeft(new ibis::math::bediener(operador));
		ptr = reinterpret_cast<ibis::math::term*>(ptr->getLeft());
	    }
	}
	ptr->setLeft(terms[j]);
    }
#if _DEBUG+0 > 1 || DEBUG+0 > 1
    LOGGER(ibis::gVerbose > 6)
	<< "DEBUG -- bediener::reorder output:  " << *this;
#endif
} // ibis::math::bediener::reorder

void ibis::math::bediener::linearize
(const ibis::math::OPERADOR op,
 std::vector<ibis::math::term*>& terms) {
    if (operador == op) {
	ibis::math::term* rhs = reinterpret_cast<ibis::math::term*>
	    (getRight());
	if (rhs->termType() == ibis::math::OPERATOR &&
	    reinterpret_cast<ibis::math::bediener*>(rhs)->operador == op)
	    reinterpret_cast<ibis::math::bediener*>(rhs)
		->linearize(op, terms);
	else
	    terms.push_back(rhs->dup());

	ibis::math::term* lhs = reinterpret_cast<ibis::math::term*>
	    (getLeft());
	if (lhs->termType() == ibis::math::OPERATOR &&
	    reinterpret_cast<ibis::math::bediener*>(lhs)->operador == op)
	    reinterpret_cast<ibis::math::bediener*>(lhs)
		->linearize(op, terms);
	else
	    terms.push_back(lhs->dup());
    }
} // ibis::math::bediener::linearize

// if the right operand is a number, there are two cases where we can
// change the operators
void ibis::math::bediener::convertConstants() {
#if _DEBUG+0 > 1 || DEBUG+0 > 1
    LOGGER(ibis::gVerbose > 8)
	<< "DEBUG -- bediener::convertConstants  input:  " << *this;
#endif
    ibis::math::term* rhs = reinterpret_cast<ibis::math::term*>
	(getRight());
    if (rhs->termType() == ibis::math::NUMBER) {
	if (operador == ibis::math::MINUS) {
	    reinterpret_cast<ibis::math::number*>(rhs)->negate();
	    operador = ibis::math::PLUS;

	    ibis::math::term* lhs =
		reinterpret_cast<ibis::math::term*>(getLeft());
	    if (lhs->termType() == ibis::math::OPERATOR)
		reinterpret_cast<ibis::math::bediener*>(lhs)
		    ->convertConstants();
	}
	else if (operador == ibis::math::DIVIDE) {
	    reinterpret_cast<ibis::math::number*>(rhs)->invert();
	    operador = ibis::math::MULTIPLY;

	    ibis::math::term* lhs =
		reinterpret_cast<ibis::math::term*>(getLeft());
	    if (lhs->termType() == ibis::math::OPERATOR)
		reinterpret_cast<ibis::math::bediener*>(lhs)
		    ->convertConstants();
	}
    }
#if _DEBUG+0 > 1 || DEBUG+0 > 1
    LOGGER(ibis::gVerbose > 8)
	<< "DEBUG -- bediener::convertConstants output:  " << *this;
#endif
} // ibis::math::convertConstants

void ibis::math::stdFunction1::print(std::ostream& out) const {
    out << stdfun1_name[ftype] << '(';
    getLeft()->print(out);
    out << ')';
} // ibis::math::stdFunction1::print

void ibis::math::stdFunction2::print(std::ostream& out) const {
    out << stdfun2_name[ftype] << '(';
    getLeft()->print(out);
    out << ", ";
    getRight()->print(out);
    out << ')';
} // ibis::math::stdFunction2::print

void ibis::compRange::print(std::ostream& out) const {
    switch (op12) {
    case OP_EQ:
	getLeft()->print(out);
	out << " == ";
	break;
    case OP_LT:
	getLeft()->print(out);
	out << " < ";
	break;
    case OP_LE:
	getLeft()->print(out);
	out << " <= ";
	break;
    case OP_GT:
	getLeft()->print(out);
	out << " > ";
	break;
    case OP_GE:
	getLeft()->print(out);
	out << " >= ";
	break;
    default: break;
    }
    getRight()->print(out);
    if (expr3) {
	switch (op23) {
	case OP_EQ:
	    out << " == ";
	    expr3->print(out);
	    break;
	case OP_LT:
	    out << " < ";
	    expr3->print(out);
	    break;
	case OP_LE:
	    out << " <= ";
	    expr3->print(out);
	    break;
	case OP_GT:
	    out << " > ";
	    expr3->print(out);
	    break;
	case OP_GE:
	    out << " >= ";
	    expr3->print(out);
	    break;
	default: break;
	}
    }
} // ibis::compRange::print

// convert to a simple range stored as ibis::qContinuousRange
// attempt to replace the operators > and >= with < and <=
ibis::qContinuousRange* ibis::compRange::simpleRange() const {
    ibis::qContinuousRange* res = 0;
    if (expr3 == 0) {
	if (reinterpret_cast<const ibis::math::term*>(getLeft())->
	    termType()==ibis::math::VARIABLE &&
	    reinterpret_cast<const ibis::math::term*>(getRight())->
	    termType()==ibis::math::NUMBER) {
	    res = new ibis::qContinuousRange
		(reinterpret_cast<const ibis::math::variable*>
		 (getLeft())->variableName(), op12,
		 reinterpret_cast<const ibis::math::term*>
		 (getRight())->eval());
	}
	else if (reinterpret_cast<const ibis::math::term*>(getLeft())->
		 termType()==ibis::math::NUMBER &&
		 reinterpret_cast<const ibis::math::term*>(getRight())->
		 termType()==ibis::math::VARIABLE) {
	    switch (op12) {
	    case ibis::qExpr::OP_LT:
		res = new ibis::qContinuousRange
		    (reinterpret_cast<const ibis::math::variable*>
		     (getRight())->variableName(), ibis::qExpr::OP_GT,
		     reinterpret_cast<const ibis::math::term*>
		     (getLeft())->eval());
		break;
	    case ibis::qExpr::OP_LE:
		res = new ibis::qContinuousRange
		    (reinterpret_cast<const ibis::math::variable*>
		     (getRight())->variableName(), ibis::qExpr::OP_GE,
		     reinterpret_cast<const ibis::math::term*>
		     (getLeft())->eval());
		break;
	    case ibis::qExpr::OP_GT:
		res = new ibis::qContinuousRange
		    (reinterpret_cast<const ibis::math::variable*>
		     (getRight())->variableName(), ibis::qExpr::OP_LT,
		     reinterpret_cast<const ibis::math::term*>
		     (getLeft())->eval());
		break;
	    case ibis::qExpr::OP_GE:
		res = new ibis::qContinuousRange
		    (reinterpret_cast<const ibis::math::variable*>
		     (getRight())->variableName(), ibis::qExpr::OP_LE,
		     reinterpret_cast<const ibis::math::term*>
		     (getLeft())->eval());
		break;
	    default:
		res = new ibis::qContinuousRange
		    (reinterpret_cast<const ibis::math::variable*>
		     (getRight())->variableName(), op12,
		     reinterpret_cast<const ibis::math::term*>
		     (getLeft())->eval());
		break;
	    }
	}
    }
    else if (expr3->termType() == ibis::math::NUMBER &&
	     reinterpret_cast<const ibis::math::term*>(getLeft())->
	     termType()==ibis::math::NUMBER &&
	     reinterpret_cast<const ibis::math::term*>(getRight())->
	     termType()==ibis::math::VARIABLE) {
	const char* vname =
	    reinterpret_cast<const ibis::math::variable*>
	    (getRight())->variableName();
	double val0 = reinterpret_cast<const ibis::math::number*>
	    (getLeft())->eval();
	double val1 = expr3->eval();
	switch (op12) {
	case ibis::qExpr::OP_LT:
	    switch (op23) {
	    case ibis::qExpr::OP_LT:
	    case ibis::qExpr::OP_LE:
		res = new ibis::qContinuousRange
		    (val0, op12, vname, op23, val1);
		break;
	    case ibis::qExpr::OP_GT:
		if (val0 >= val1)
		    res = new ibis::qContinuousRange
			(vname, ibis::qExpr::OP_GT, val0);
		else
		    res = new ibis::qContinuousRange
			(vname, ibis::qExpr::OP_GT, val1);
		break;
	    case ibis::qExpr::OP_GE:
		if (val0 >= val1)
		    res = new ibis::qContinuousRange
			(vname, ibis::qExpr::OP_GT, val0);
		else
		    res = new ibis::qContinuousRange
			(vname, ibis::qExpr::OP_GE, val1);
		break;
	    case ibis::qExpr::OP_EQ:
		if (val1 > val0)
		    res = new ibis::qContinuousRange(vname, op23, val1);
		else
		    res = new ibis::qContinuousRange;
		break;
	    default:
		res = new ibis::qContinuousRange;
		break;
	    }
	    break;
	case ibis::qExpr::OP_LE:
	    switch (op23) {
	    case ibis::qExpr::OP_LT:
	    case ibis::qExpr::OP_LE:
		res = new ibis::qContinuousRange
		    (val0, op12, vname, op23, val1);
		break;
	    case ibis::qExpr::OP_GT:
		if (val0 > val1)
		    res = new ibis::qContinuousRange
			(vname, ibis::qExpr::OP_GE, val0);
		else
		    res = new ibis::qContinuousRange
			(vname, ibis::qExpr::OP_GT, val1);
		break;
	    case ibis::qExpr::OP_GE:
		if (val0 >= val1)
		    res = new ibis::qContinuousRange
			(vname, ibis::qExpr::OP_GE, val0);
		else
		    res = new ibis::qContinuousRange
			(vname, ibis::qExpr::OP_GE, val1);
		break;
	    case ibis::qExpr::OP_EQ:
		if (val1 >= val0)
		    res = new ibis::qContinuousRange(vname, op23, val1);
		else
		    res = new ibis::qContinuousRange;
		break;
	    default:
		res = new ibis::qContinuousRange;
		break;
	    }
	    break;
	case ibis::qExpr::OP_GT:
	    switch (op23) {
	    case ibis::qExpr::OP_LT:
		if (val0 >= val1)
		    res = new ibis::qContinuousRange
			(vname, ibis::qExpr::OP_LT, val1);
		else
		    res = new ibis::qContinuousRange
			(vname, ibis::qExpr::OP_LT, val0);
		break;
	    case ibis::qExpr::OP_LE:
		if (val0 >= val1)
		    res = new ibis::qContinuousRange
			(vname, ibis::qExpr::OP_LT, val0);
		else
		    res = new ibis::qContinuousRange
			(vname, ibis::qExpr::OP_LE, val1);
		break;
	    case ibis::qExpr::OP_GT:
		res = new ibis::qContinuousRange
		    (val1, ibis::qExpr::OP_LT, vname,
		     ibis::qExpr::OP_LT, val0);
		break;
	    case ibis::qExpr::OP_GE:
		res = new ibis::qContinuousRange
		    (val1, ibis::qExpr::OP_LE, vname,
		     ibis::qExpr::OP_LT, val0);
		break;
	    case ibis::qExpr::OP_EQ:
		if (val1 < val0)
		    res = new ibis::qContinuousRange(vname, op23, val1);
		else
		    res = new ibis::qContinuousRange;
		break;
	    default:
		res = new ibis::qContinuousRange;
		break;
	    }
	    break;
	case ibis::qExpr::OP_GE:
	    switch (op23) {
	    case ibis::qExpr::OP_LT:
		if (val0 >= val1)
		    res = new ibis::qContinuousRange
			(vname, ibis::qExpr::OP_LT, val1);
		else
		    res = new ibis::qContinuousRange
			(vname, ibis::qExpr::OP_LE, val0);
		break;
	    case ibis::qExpr::OP_LE:
		if (val0 >= val1)
		    res = new ibis::qContinuousRange
			(vname, ibis::qExpr::OP_LE, val0);
		else
		    res = new ibis::qContinuousRange
			(vname, ibis::qExpr::OP_LE, val1);
		break;
	    case ibis::qExpr::OP_GT:
		res = new ibis::qContinuousRange
		    (val1, ibis::qExpr::OP_LT, vname,
		     ibis::qExpr::OP_LE, val0);
		break;
	    case ibis::qExpr::OP_GE:
		res = new ibis::qContinuousRange
		    (val1, ibis::qExpr::OP_LE, vname,
		     ibis::qExpr::OP_LE, val0);
		break;
	    case ibis::qExpr::OP_EQ:
		if (val1 <= val0)
		    res = new ibis::qContinuousRange(vname, op23, val1);
		else
		    res = new ibis::qContinuousRange;
		break;
	    default:
		res = new ibis::qContinuousRange;
		break;
	    }
	    break;
	case ibis::qExpr::OP_EQ:
	    switch (op23) {
	    case ibis::qExpr::OP_LT:
		if (val0 < val1)
		    res = new ibis::qContinuousRange(vname, op12, val0);
		else
		    res = new ibis::qContinuousRange;
		break;
	    case ibis::qExpr::OP_LE:
		if (val0 <= val1)
		    res = new ibis::qContinuousRange(vname, op12, val0);
		else
		    res = new ibis::qContinuousRange;
		break;
	    case ibis::qExpr::OP_GT:
		if (val1 < val0)
		    res = new ibis::qContinuousRange(vname, op12, val0);
		else
		    res = new ibis::qContinuousRange;
		break;
	    case ibis::qExpr::OP_GE:
		if (val1 <= val0)
		    res = new ibis::qContinuousRange(vname, op12, val0);
		else
		    res = new ibis::qContinuousRange;
		break;
	    case ibis::qExpr::OP_EQ:
		if (val1 == val0)
		    res = new ibis::qContinuousRange(vname, op12, val0);
		else
		    res = new ibis::qContinuousRange;
		break;
	    default:
		res = new ibis::qContinuousRange;
		break;
	    }
	    break;
	default:
	    res = new ibis::qContinuousRange;
	    break;
	}
    }
    return res;
} // ibis::compRange::simpleRange

ibis::qDiscreteRange::qDiscreteRange(const char *col, const char *nums)
    : ibis::qRange(ibis::qExpr::DRANGE) {
    if (col == 0 || *col == 0) return;
    name = col;
    if (nums == 0 || *nums == 0) return;
    // use a std::set to temporarily hold the values and eliminate
    // duplicates
    std::set<double> dset;
    const char *str = nums;
    while (*str != 0) {
	char *stmp;
	double dtmp = strtod(str, &stmp);
	if (stmp > str) {// get a value, maybe HUGE_VAL, INF, NAN
#if defined(_WIN32) && !defined(__CYGWIN__)
	    if (_finite(dtmp))
#else
	    if (finite(dtmp))
#endif
		dset.insert(dtmp);
	    str = stmp + strspn(stmp, "\n\v\t, ");
	}
	else { // invalid value, skip to next space
	    const char* st = strpbrk(str, "\n\v\t, ");
	    if (st != 0)
		str = st + strspn(st, "\n\v\t, ");
	    else
		str = st;
	}
    }
    if (! dset.empty()) {
	values.reserve(dset.size());
	for (std::set<double>::const_iterator it = dset.begin();
	     it != dset.end(); ++ it)
	    values.push_back(*it);
    }
} // qDiscreteRange ctor

/// Construct a qDiscreteRange object from a vector of unsigned 32-bit integers.
ibis::qDiscreteRange::qDiscreteRange(const char *col,
				     const std::vector<uint32_t>& val)
    : ibis::qRange(ibis::qExpr::DRANGE) {
    if (col == 0 || *col == 0) return;
    name = col;
    if (val.empty()) return;
    if (val.size() == 1) {
	values.resize(1);
	values[0] = val[0];
	return;
    }

#if 0
    { // use a std::set to temporarily hold the values and eliminate
      // duplicates
	std::set<uint32_t> dset;
	for (std::vector<uint32_t>::const_iterator it = val.begin();
	     it != val.end(); ++ it)
	    dset.insert(*it);

	if (! dset.empty()) {
	    values.reserve(dset.size());
	    for (std::set<uint32_t>::const_iterator it = dset.begin();
		 it != dset.end(); ++ it)
		values.push_back(*it);
	}
    }
#else
    { // copy the incoming numbers into array tmp and sort them before
      // passing them to values
	std::vector<uint32_t> tmp(val);
	std::sort(tmp.begin(), tmp.end());
	size_t j = 0;
	for (size_t i = 1; i < tmp.size(); ++ i) {
	    if (tmp[i] > tmp[j]) {
		++ j;
		tmp[j] = tmp[i];
	    }
	}
	tmp.resize(j+1);
	values.resize(tmp.size());
	std::copy(tmp.begin(), tmp.end(), values.begin());
    }
#endif
    if (values.size() < val.size()) {
	unsigned j = val.size() - values.size();
	LOGGER(ibis::gVerbose > 1)
	    << "ibis::qDiscreteRange::ctor accepted incoming int array with "
	    << val.size() << " elements, removed " << j
	    << " duplicate value" << (j > 1 ? "s" : "");
    }
} // qDiscreteRange ctor

/// Construct a qDiscreteRange object from a vector of double values.
ibis::qDiscreteRange::qDiscreteRange(const char *col,
				     const std::vector<double>& val)
    : ibis::qRange(ibis::qExpr::DRANGE), name(col), values(val) {
    if (val.size() <= 1U) return;

    bool sorted = (values[0] <= values[1]);
    for (size_t i = 1; sorted && i < val.size()-1; ++ i)
	sorted = (values[i] <= values[i+1]);
    if (sorted == false) {
	/// Sort the incoming values and remove duplicates.
	std::sort(values.begin(), values.end());
    }
    size_t j = 0;
    for (size_t i = 1; i < val.size(); ++ i) {
	// loop to copy unique values to the beginning of the array
	if (values[i] > values[j]) {
	    ++ j;
	    values[j] = values[i];
	}
    }
    ++ j;
    values.resize(j);
    if (j < val.size()) {
	j = val.size() - j;
	LOGGER(ibis::gVerbose > 1)
	    << "ibis::qDiscreteRange::ctor accepted incoming double array with "
	    << val.size() << " elements, removed " << j
	    << " duplicate value" << (j > 1 ? "s" : "");
    }
} // ibis::qDiscreteRange::qDiscreteRange

/// Construct a qDiscreteRange object from an array of double values.
ibis::qDiscreteRange::qDiscreteRange(const char *col,
				     const array_t<double>& val)
    : ibis::qRange(ibis::qExpr::DRANGE), name(col) {
    if (val.empty()) return;
    values.resize(val.size());
    values[0] = val[0];
    if (val.size() <= 1U) return;

    bool sorted = (val[0] <= val[1]);
    values[1] = val[1];
    for (size_t i = 2; i < val.size(); ++ i) {
	values[i] = val[i];
	sorted = sorted && (val[i-1] <= val[i]);
    }

    if (sorted == false) {
	/// Sort the incoming values and remove duplicates.
	std::sort(values.begin(), values.end());
    }
    size_t j = 0;
    for (size_t i = 1; i < val.size(); ++ i) {
	// loop to copy unique values to the beginning of the array
	if (values[i] > values[j]) {
	    ++ j;
	    values[j] = values[i];
	}
    }
    ++ j;
    values.resize(j);
    if (j < val.size()) {
	j = val.size() - j;
	LOGGER(ibis::gVerbose > 1)
	    << "ibis::qDiscreteRange::ctor accepted incoming double array with "
	    << val.size() << " elements, removed " << j
	    << " duplicate value" << (j > 1 ? "s" : "");
    }
} // ibis::qDiscreteRange::qDiscreteRange

void ibis::qDiscreteRange::print(std::ostream& out) const {
    out << name << " IN (";
//     std::copy(values.begin(), values.end(),
// 	      std::ostream_iterator<double>(out, ", "));
    if (values.size() > 0) {
	size_t prt = ((values.size() >> ibis::gVerbose) > 1) ?
	    (1U << ibis::gVerbose) : values.size();
	if (prt == 0)
	    prt = 1;
	else if (prt+prt >= values.size())
	    prt = values.size();
	out << values[0];
	for (uint32_t i = 1; i < prt; ++ i)
	    out << ", " << values[i];
	if (prt < values.size())
	    out << " ... " << values.size()-prt << " omitted";
    }
    out << ')';
} // ibis::qDiscreteRange::print

void ibis::qDiscreteRange::printRange(std::ostream& out) const {
    for (uint32_t i = 0; i < values.size(); ++ i) {
	int64_t ival = static_cast<int64_t>(values[i]);
	out << name << "\t";
	if (ival == values[i])
	    out << ival << "\n";
	else
	    out << values[i] << "\n";
    }
} // ibis::qDiscreteRange::printRange

ibis::qExpr* ibis::qDiscreteRange::convert() const {
    if (name.empty()) return 0;
    if (values.empty()) { // an empty range
	ibis::qContinuousRange *ret = new ibis::qContinuousRange
	    (0.0, OP_LE, name.c_str(), OP_LT, -1.0);
	return ret;
    }

    ibis::qExpr *ret = new ibis::qContinuousRange
	(name.c_str(), ibis::qExpr::OP_EQ, values[0]);
    for (uint32_t i = 1; i < values.size(); ++ i) {
	ibis::qExpr *rhs = new ibis::qContinuousRange
	    (name.c_str(), ibis::qExpr::OP_EQ, values[i]);
	ibis::qExpr *op = new ibis::qExpr(ibis::qExpr::LOGICAL_OR);
	op->setRight(rhs);
	op->setLeft(ret);
	ret = op;
    }
    return ret;
} // ibis::qDiscreteRange::convert

void ibis::qDiscreteRange::restrictRange(double left, double right) {
    if (left > right)
	return;
    uint32_t start = 0;
    uint32_t size = values.size();
    while (start < size && values[start] < left)
	++ start;

    uint32_t sz;
    if (start > 0) { // need to copy values
	for (sz = 0; sz+start < size && values[sz+start] <= right; ++ sz)
	    values[sz] = values[sz+start];
    }
    else {
	for (sz = 0; sz < size && values[sz] <= right; ++ sz);
    }
    values.resize(sz);
} // ibis::qDiscreteRange::restrictRange

ibis::qMultiString::qMultiString(const char *col, const char *sval)
    : ibis::qExpr(ibis::qExpr::MSTRING) {
    if (col == 0 || *col == 0) return;
    name = col;
    if (sval == 0 || *sval == 0) return;
    std::set<std::string> sset; // use it to sort and remove duplicate
    while (*sval != 0) {
	std::string tmp;
	tmp.erase();
	while (*sval && isspace(*sval)) ++ sval; // skip leading space
	if (*sval == '\'') { // single quoted string
	    ++ sval;
	    while (*sval) {
		if (*sval != '\'')
		    tmp += *sval;
		else if (tmp.size() > 0 && tmp[tmp.size()-1] == '\\')
		    // escaped quote
		    tmp[tmp.size()-1] = '\'';
		else {// found the end quote
		    ++ sval; // skip the closing quote
		    break;
		}
		++ sval;
	    }
	    if (! tmp.empty())
		sset.insert(tmp);
	}
	else if (*sval == '"') { // double quoted string
	    ++ sval;
	    while (*sval) {
		if (*sval != '"')
		    tmp += *sval;
		else if (tmp.size() > 0 && tmp[tmp.size()-1] == '\\')
		    // escaped quote
		    tmp[tmp.size()-1] = '"';
		else {
		    ++ sval; // skip the closing quote
		    break;
		}
		++ sval;
	    }
	    if (! tmp.empty())
		sset.insert(tmp);
	}
	else { // space and comma delimited string
	    while (*sval) {
		if (*sval != ',' && ! isspace(*sval))
		    tmp += *sval;
		else if (tmp[tmp.size()-1] == '\\')
		    tmp[tmp.size()-1] = *sval;
		else
		    break;
		++ sval;
	    }
	    if (! tmp.empty())
		sset.insert(tmp);
	}
	if (*sval != 0)
	    sval += strspn(sval, "\n\v\t, ");
    }

    if (! sset.empty()) {
	values.reserve(sset.size());
	for (std::set<std::string>::const_iterator it = sset.begin();
	     it != sset.end(); ++ it)
	    values.push_back(*it);
    }
} // ibis::qMultiString ctor

void ibis::qMultiString::print(std::ostream& out) const {
    if (name.empty()) return;
    out << name << " IN (";
//     std::copy(values.begin(), values.end(),
// 	      std::ostream_iterator<std::string>(out, ", "));
    if (values.size() > 0) {
	out << values[0];
	for (uint32_t i = 1; i < values.size(); ++ i)
	    out << ", " << values[i];
    }
    out << ')';
} // ibis::qMultiString::print

ibis::qExpr* ibis::qMultiString::convert() const {
    if (name.empty()) return 0;
    if (values.empty()) return 0;

    ibis::qExpr *ret = new ibis::qString(name.c_str(), values[0].c_str());
    for (uint32_t i = 1; i < values.size(); ++ i) {
	ibis::qExpr *rhs = new ibis::qString(name.c_str(), values[i].c_str());
	ibis::qExpr *op = new ibis::qExpr(ibis::qExpr::LOGICAL_OR);
	op->setRight(rhs);
	op->setLeft(ret);
	ret = op;
    }
    return ret;
} // ibis::qMultiString::convert

void ibis::rangeJoin::print(std::ostream& out) const {
    out << "join(" << name1 << ", " << name2;
    if (expr)
	out << ", " << *expr;
    out << ')';
} // ibis::rangeJoin::print

/// Constructing a qAnyAny object from a string and a floating-point value.
ibis::qAnyAny::qAnyAny(const char *pre, const double dbl)
    : ibis::qExpr(ibis::qExpr::ANYANY), prefix(pre) {
    values.resize(1);
    values[0] = dbl;
}

/// Constructing an object of type qAnyAny from two strings.  The second
/// string is expected to be a list of numbers separated by coma and space.
ibis::qAnyAny::qAnyAny(const char *pre, const char *val)
    : ibis::qExpr(ibis::qExpr::ANYANY), prefix(pre) {
    // use a std::set to temporarily hold the values and eliminate
    // duplicates
    std::set<double> dset;
    const char *str = val + (*val=='(');
    while (*str != 0) {
	char *stmp;
	double dtmp = strtod(str, &stmp);
	if (stmp > str) {// get a value, maybe HUGE_VAL, INF, NAN
#if defined(_WIN32) && !defined(__CYGWIN__)
	    if (_finite(dtmp))
#else
	    if (finite(dtmp))
#endif
		dset.insert(dtmp);
	    str = stmp + strspn(stmp, "\n\v\t, ");
	}
	else { // invalid value, skip to next space
	    const char *st1 = strpbrk(str, "\n\v\t, ");
	    if (st1 != 0)
		str = st1 + strspn(st1, "\n\v\t, ");
	    else
		str = st1;
	}
    }
    if (! dset.empty()) {
	values.reserve(dset.size());
	for (std::set<double>::const_iterator it = dset.begin();
	     it != dset.end(); ++ it)
	    values.push_back(*it);
    }
} // ibis::qAnyAny

void ibis::qAnyAny::print(std::ostream& out) const {
    if (values.size() > 1) {
	out << "ANY(" << prefix << ") IN (";
	if (values.size() > 0) {
	    out << values[0];
	    for (uint32_t i = 1; i < values.size(); ++ i)
		out << ", " << values[i];
	}
	out << ')';
    }
    else if (values.size() == 1) {
	out << "ANY(" << prefix << ")==" << values.back();
    }
} // ibis::qAnyAny::print

void ibis::qAnyAny::printRange(std::ostream& out) const {
    for (uint32_t i = 0; i < values.size(); ++ i) {
	int64_t ival = static_cast<int64_t>(values[i]);
	out << prefix << "*\t";
	if (ival == values[i])
	    out << ival << "\n";
	else
	    out << values[i] << "\n";
    }
} // ibis::qAnyAny::printRange