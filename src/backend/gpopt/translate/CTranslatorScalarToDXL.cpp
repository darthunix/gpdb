//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2011 Greenplum, Inc.
//
//	@filename:
//		CTranslatorScalarToDXL.cpp
//
//	@doc:
//		Implementing the methods needed to translate a GPDB Scalar Operation (in a Query / PlStmt object)
//		into a DXL trees
//
//	@test:
//
//---------------------------------------------------------------------------

#include "postgres.h"
#include "nodes/plannodes.h"
#include "nodes/parsenodes.h"
#include "nodes/primnodes.h"
#include "utils/datum.h"
#include "utils/date.h"
#include "utils/numeric.h"
#include "catalog/pg_proc.h"

#include "gpopt/translate/CTranslatorScalarToDXL.h"
#include "gpopt/translate/CTranslatorQueryToDXL.h"
#include "gpopt/translate/CTranslatorUtils.h"
#include "gpopt/translate/CCTEListEntry.h"

#include "gpos/base.h"
#include "gpos/common/CAutoP.h"
#include "gpos/string/CWStringDynamic.h"

#include "naucrates/dxl/CDXLUtils.h"
#include "naucrates/dxl/xml/dxltokens.h"

#include "gpopt/base/CUtils.h"
#include "gpopt/mdcache/CMDAccessor.h"

#include "naucrates/dxl/operators/CDXLDatumBool.h"
#include "naucrates/dxl/operators/CDXLDatumInt2.h"
#include "naucrates/dxl/operators/CDXLDatumInt4.h"
#include "naucrates/dxl/operators/CDXLDatumInt8.h"
#include "naucrates/dxl/operators/CDXLDatumOid.h"

#include "naucrates/md/IMDAggregate.h"
#include "naucrates/md/IMDScalarOp.h"
#include "naucrates/md/IMDType.h"
#include "naucrates/md/CMDTypeGenericGPDB.h"

#include "gpopt/gpdbwrappers.h"

using namespace gpdxl;
using namespace gpopt;


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::CTranslatorScalarToDXL
//
//	@doc:
//		Ctor
//---------------------------------------------------------------------------
CTranslatorScalarToDXL::CTranslatorScalarToDXL
	(
	CMemoryPool *mp,
	CMDAccessor *md_accessor,
	CIdGenerator *colid_generator,
	CIdGenerator *cte_id_generator,
	ULONG query_level,
	BOOL is_query_mode,
	HMUlCTEListEntry *cte_entries,
	CDXLNodeArray *cte_dxlnode_array
	)
	:
	m_mp(mp),
	m_md_accessor(md_accessor),
	m_colid_generator(colid_generator),
	m_cte_id_generator(cte_id_generator),
	m_query_level(query_level),
	m_has_distributed_tables(false),
	m_is_query_mode(is_query_mode),
	m_op_type(EpspotNone),
	m_cte_entries(cte_entries),
	m_cte_producers(cte_dxlnode_array)
{
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::EdxlbooltypeFromGPDBBoolType
//
//	@doc:
//		Return the EdxlBoolExprType for a given GPDB BoolExprType
//---------------------------------------------------------------------------
EdxlBoolExprType
CTranslatorScalarToDXL::EdxlbooltypeFromGPDBBoolType
	(
	BoolExprType boolexprtype
	)
	const
{
	static ULONG mapping[][2] =
		{
		{NOT_EXPR, Edxlnot},
		{AND_EXPR, Edxland},
		{OR_EXPR, Edxlor},
		};

	EdxlBoolExprType type = EdxlBoolExprTypeSentinel;

	const ULONG arity = GPOS_ARRAY_SIZE(mapping);
	for (ULONG ul = 0; ul < arity; ul++)
	{
		ULONG *elem = mapping[ul];
		if ((ULONG) boolexprtype == elem[0])
		{
			type = (EdxlBoolExprType) elem[1];
			break;
		}
	}

	GPOS_ASSERT(EdxlBoolExprTypeSentinel != type && "Invalid bool expr type");

	return type;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateVarToDXL
//
//	@doc:
//		Create a DXL node for a scalar ident expression from a GPDB Var expression.
//		This function can be used for constructing a scalar ident operator appearing in a
//		base node (e.g. a scan node) or in an intermediate plan nodes.
//		The function allocates memory in the translator memory pool, and the caller
//		is responsible for freeing it
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::TranslateVarToDXL
	(
	const Expr *expr,
	const CMappingVarColId* var_colid_mapping
	)
{
	GPOS_ASSERT(IsA(expr, Var));
	const Var * var = (Var *) expr;

	if (var->varattno == 0)
	{
		GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, GPOS_WSZ_LIT("Whole-row variable"));
	}

	// column name
	const CWStringBase *str = var_colid_mapping->GetOptColName(m_query_level, var, m_op_type);

	// column id
	ULONG id;

	if(var->varattno != 0 || EpspotIndexScan == m_op_type || EpspotIndexOnlyScan == m_op_type)
	{
		id = var_colid_mapping->GetColId(m_query_level, var, m_op_type);
	}
	else
	{
		id = m_colid_generator->next_id();
	}
	CMDName *mdname = GPOS_NEW(m_mp) CMDName(m_mp, str);

	// create a column reference for the given var
	CDXLColRef *dxl_colref = GPOS_NEW(m_mp) CDXLColRef
												(
												m_mp,
												mdname,
												id,
												GPOS_NEW(m_mp) CMDIdGPDB(var->vartype),
												var->vartypmod
												);

	// create the scalar ident operator
	CDXLScalarIdent *scalar_ident = GPOS_NEW(m_mp) CDXLScalarIdent
													(
													m_mp,
													dxl_colref
													);

	// create the DXL node holding the scalar ident operator
	CDXLNode *dxlnode = GPOS_NEW(m_mp) CDXLNode(m_mp, scalar_ident);

	return dxlnode;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateScalarToDXL
//
//	@doc:
//		Create a DXL node for a scalar expression from a GPDB expression node.
//		This function can be used for constructing a scalar operator appearing in a
//		base node (e.g. a scan node), or in an intermediate plan nodes.
//		The function allocates memory in the translator memory pool, and the caller
//		is responsible for freeing it
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::TranslateScalarToDXL
	(
	const Expr *expr,
	const CMappingVarColId* var_colid_mapping,
	BOOL *has_distributed_tables // output
	)
{
	static const STranslatorElem translators[] =
	{
		{T_Var, &CTranslatorScalarToDXL::TranslateVarToDXL},
		{T_OpExpr, &CTranslatorScalarToDXL::TranslateOpExprToDXL},
		{T_ScalarArrayOpExpr, &CTranslatorScalarToDXL::TranslateScalarArrayOpExprToDXL},
		{T_DistinctExpr, &CTranslatorScalarToDXL::TranslateDistinctExprToDXL},
		{T_Const, &CTranslatorScalarToDXL::TranslateConstToDXL},
		{T_BoolExpr, &CTranslatorScalarToDXL::TranslateBoolExprToDXL},
		{T_BooleanTest, &CTranslatorScalarToDXL::TranslateBooleanTestToDXL},
		{T_CaseExpr, &CTranslatorScalarToDXL::TranslateCaseExprToDXL},
		{T_CaseTestExpr, &CTranslatorScalarToDXL::TranslateCaseTestExprToDXL},
		{T_CoalesceExpr, &CTranslatorScalarToDXL::TranslateCoalesceExprToDXL},
		{T_MinMaxExpr, &CTranslatorScalarToDXL::TranslateMinMaxExprToDXL},
		{T_FuncExpr, &CTranslatorScalarToDXL::TranslateFuncExprToDXL},
		{T_Aggref, &CTranslatorScalarToDXL::TranslateAggrefToDXL},
		{T_WindowRef, &CTranslatorScalarToDXL::TranslateWindowRefToDXL},
		{T_NullTest, &CTranslatorScalarToDXL::TranslateNullTestToDXL},
		{T_NullIfExpr, &CTranslatorScalarToDXL::TranslateNullIfExprToDXL},
		{T_RelabelType, &CTranslatorScalarToDXL::TranslateRelabelTypeToDXL},
		{T_CoerceToDomain, &CTranslatorScalarToDXL::TranslateCoerceToDomainToDXL},
		{T_CoerceViaIO, &CTranslatorScalarToDXL::TranslateCoerceViaIOToDXL},
		{T_ArrayCoerceExpr, &CTranslatorScalarToDXL::TranslateArrayCoerceExprToDXL},
		{T_SubLink, &CTranslatorScalarToDXL::TranslateSubLinkToDXL},
		{T_ArrayExpr, &CTranslatorScalarToDXL::TranslateArrayExprToDXL},
		{T_ArrayRef, &CTranslatorScalarToDXL::TranslateArrayRefToDXL},
	};

	const ULONG num_translators = GPOS_ARRAY_SIZE(translators);
	NodeTag tag = expr->type;

	// if an output variable is provided, we need to reset the member variable
	if (NULL != has_distributed_tables)
	{
		m_has_distributed_tables = false;
	}

	// save old value for distributed tables flag
	BOOL has_distributed_tables_old = m_has_distributed_tables;

	// find translator for the expression type
	ExprToDXLFn func_ptr = NULL;
	for (ULONG ul = 0; ul < num_translators; ul++)
	{
		STranslatorElem elem = translators[ul];
		if (tag == elem.tag)
		{
			func_ptr = elem.func_ptr;
			break;
		}
	}

	if (NULL == func_ptr)
	{
		CHAR *str = (CHAR*) gpdb::NodeToString(const_cast<Expr*>(expr));
		CWStringDynamic *wcstr = CDXLUtils::CreateDynamicStringFromCharArray(m_mp, str);
		GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiPlStmt2DXLConversion, wcstr->GetBuffer());
	}

	CDXLNode *return_node = (this->*func_ptr)(expr, var_colid_mapping);

	// combine old and current values for distributed tables flag
	m_has_distributed_tables = m_has_distributed_tables || has_distributed_tables_old;

	if (NULL != has_distributed_tables && m_has_distributed_tables)
	{
		*has_distributed_tables = true;
	}

	return return_node;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateDistinctExprToDXL
//
//	@doc:
//		Create a DXL node for a scalar distinct comparison expression from a GPDB
//		DistinctExpr.
//		This function can be used for constructing a scalar comparison operator appearing in a
//		base node (e.g. a scan node), or in an intermediate plan nodes.
//		The function allocates memory in the translator memory pool, and the caller
//		is responsible for freeing it
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::TranslateDistinctExprToDXL
	(
	const Expr *expr,
	const CMappingVarColId* var_colid_mapping
	)
{
	GPOS_ASSERT(IsA(expr, DistinctExpr));
	const DistinctExpr *distinct_expr = (DistinctExpr *) expr;

	// process arguments of op expr
	GPOS_ASSERT(2 == gpdb::ListLength(distinct_expr->args));

	CDXLNode *left_node = TranslateScalarToDXL
							(
							(Expr *) gpdb::ListNth(distinct_expr->args, 0),
							var_colid_mapping
							);

	CDXLNode *right_node = TranslateScalarToDXL
							(
							(Expr *) gpdb::ListNth(distinct_expr->args, 1),
							var_colid_mapping
							);

	GPOS_ASSERT(NULL != left_node);
	GPOS_ASSERT(NULL != right_node);

	CDXLScalarDistinctComp *dxlop = GPOS_NEW(m_mp) CDXLScalarDistinctComp
														(
														m_mp,
														GPOS_NEW(m_mp) CMDIdGPDB(distinct_expr->opno)
														);

	// create the DXL node holding the scalar distinct comparison operator
	CDXLNode *dxlnode = GPOS_NEW(m_mp) CDXLNode(m_mp, dxlop);

	// add children in the right order
	dxlnode->AddChild(left_node);
	dxlnode->AddChild(right_node);

	return dxlnode;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::CreateScalarCmpFromOpExpr
//
//	@doc:
//		Create a DXL node for a scalar comparison expression from a GPDB OpExpr.
//		This function can be used for constructing a scalar comparison operator appearing in a
//		base node (e.g. a scan node), or in an intermediate plan nodes.
//		The function allocates memory in the translator memory pool, and the caller
//		is responsible for freeing it
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::CreateScalarCmpFromOpExpr
	(
	const Expr *expr,
	const CMappingVarColId* var_colid_mapping
	)
{
	GPOS_ASSERT(IsA(expr, OpExpr));
	const OpExpr *op_expr = (OpExpr *) expr;

	// process arguments of op expr
	GPOS_ASSERT(2 == gpdb::ListLength(op_expr->args));

	Expr *left_expr = (Expr *) gpdb::ListNth(op_expr->args, 0);
	Expr *right_expr = (Expr *) gpdb::ListNth(op_expr->args, 1);

	CDXLNode *left_node = TranslateScalarToDXL(left_expr, var_colid_mapping);
	CDXLNode *right_node = TranslateScalarToDXL(right_expr, var_colid_mapping);

	GPOS_ASSERT(NULL != left_node);
	GPOS_ASSERT(NULL != right_node);

	CMDIdGPDB *mdid = GPOS_NEW(m_mp) CMDIdGPDB(op_expr->opno);

	// get operator name
	const CWStringConst *str = GetDXLArrayCmpType(mdid);

	CDXLScalarComp *dxlop = GPOS_NEW(m_mp) CDXLScalarComp(m_mp, mdid, GPOS_NEW(m_mp) CWStringConst(str->GetBuffer()));

	// create the DXL node holding the scalar comparison operator
	CDXLNode *dxlnode = GPOS_NEW(m_mp) CDXLNode(m_mp, dxlop);

	// add children in the right order
	dxlnode->AddChild(left_node);
	dxlnode->AddChild(right_node);

	return dxlnode;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateOpExprToDXL
//
//	@doc:
//		Create a DXL node for a scalar opexpr from a GPDB OpExpr.
//		This function can be used for constructing a scalar opexpr operator appearing in a
//		base node (e.g. a scan node), or in an intermediate plan nodes.
//		The function allocates memory in the translator memory pool, and the caller
//		is responsible for freeing it
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::TranslateOpExprToDXL
	(
	const Expr *expr,
	const CMappingVarColId* var_colid_mapping
	)
{
	GPOS_ASSERT(IsA(expr, OpExpr));

	const OpExpr *op_expr = (OpExpr *) expr;

	// check if this is a scalar comparison
	CMDIdGPDB *return_type_mdid = GPOS_NEW(m_mp) CMDIdGPDB(((OpExpr *) expr)->opresulttype);
	const IMDType *md_type= m_md_accessor->RetrieveType(return_type_mdid);

	const ULONG num_args = gpdb::ListLength(op_expr->args);

	if (IMDType::EtiBool ==  md_type->GetDatumType() && 2 == num_args)
	{
		return_type_mdid->Release();
		return CreateScalarCmpFromOpExpr(expr, var_colid_mapping);
	}

	// get operator name and id
	IMDId *mdid = GPOS_NEW(m_mp) CMDIdGPDB(op_expr->opno);
	const CWStringConst *str = GetDXLArrayCmpType(mdid);

	CDXLScalarOpExpr *dxlop = GPOS_NEW(m_mp) CDXLScalarOpExpr(m_mp, mdid, return_type_mdid, GPOS_NEW(m_mp) CWStringConst(str->GetBuffer()));

	// create the DXL node holding the scalar opexpr
	CDXLNode *dxlnode = GPOS_NEW(m_mp) CDXLNode(m_mp, dxlop);

	// process arguments
	TranslateScalarChildren(dxlnode, op_expr->args, var_colid_mapping);

	return dxlnode;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateNullIfExprToDXL
//
//	@doc:
//		Create a DXL node for a scalar nullif from a GPDB Expr
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::TranslateNullIfExprToDXL
	(
	const Expr *expr,
	const CMappingVarColId* var_colid_mapping
	)
{
	GPOS_ASSERT(IsA(expr, NullIfExpr));
	const NullIfExpr *null_if_expr = (NullIfExpr *) expr;

	GPOS_ASSERT(2 == gpdb::ListLength(null_if_expr->args));

	CDXLScalarNullIf *dxlop = GPOS_NEW(m_mp) CDXLScalarNullIf(m_mp, GPOS_NEW(m_mp) CMDIdGPDB(null_if_expr->opno), GPOS_NEW(m_mp) CMDIdGPDB(gpdb::ExprType((Node *)null_if_expr)));

	CDXLNode *dxlnode = GPOS_NEW(m_mp) CDXLNode(m_mp, dxlop);

	// process arguments
	TranslateScalarChildren(dxlnode, null_if_expr->args, var_colid_mapping);

	return dxlnode;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateScalarArrayOpExprToDXL
//
//	@doc:
//		Create a DXL node for a scalar array expression from a GPDB OpExpr
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::TranslateScalarArrayOpExprToDXL
	(
	const Expr *expr,
	const CMappingVarColId* var_colid_mapping
	)
{
	return CreateScalarArrayCompFromExpr(expr, var_colid_mapping);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::CreateScalarArrayCompFromExpr
//
//	@doc:
//		Create a DXL node for a scalar array comparison from a GPDB OpExpr
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::CreateScalarArrayCompFromExpr
	(
	const Expr *expr,
	const CMappingVarColId* var_colid_mapping
	)
{
	GPOS_ASSERT(IsA(expr, ScalarArrayOpExpr));
	const ScalarArrayOpExpr *scalar_array_op_expr = (ScalarArrayOpExpr *) expr;

	// process arguments of op expr
	GPOS_ASSERT(2 == gpdb::ListLength(scalar_array_op_expr->args));

	Expr *left_expr = (Expr*) gpdb::ListNth(scalar_array_op_expr->args, 0);
	CDXLNode *left_node = TranslateScalarToDXL(left_expr, var_colid_mapping);

	Expr *right_expr = (Expr*) gpdb::ListNth(scalar_array_op_expr->args, 1);

	// If the argument array is an array Const, try to transform it to an
	// ArrayExpr, to allow ORCA to optimize it better. (ORCA knows how to
	// extract elements of an ArrayExpr, but doesn't currently know how
	// to do it from an array-typed Const.)
	if (IsA(right_expr, Const))
		right_expr = gpdb::TransformArrayConstToArrayExpr((Const *) right_expr);

	CDXLNode *right_node = TranslateScalarToDXL(right_expr, var_colid_mapping);

	GPOS_ASSERT(NULL != left_node);
	GPOS_ASSERT(NULL != right_node);

	// get operator name
	CMDIdGPDB *mdid_op = GPOS_NEW(m_mp) CMDIdGPDB(scalar_array_op_expr->opno);
	const IMDScalarOp *md_scalar_op = m_md_accessor->RetrieveScOp(mdid_op);
	mdid_op->Release();

	const CWStringConst *op_name = md_scalar_op->Mdname().GetMDName();
	GPOS_ASSERT(NULL != op_name);

	EdxlArrayCompType type = Edxlarraycomptypeany;

	if(!scalar_array_op_expr->useOr)
	{
		type = Edxlarraycomptypeall;
	}

	CDXLScalarArrayComp *dxlop = GPOS_NEW(m_mp) CDXLScalarArrayComp(m_mp, GPOS_NEW(m_mp) CMDIdGPDB(scalar_array_op_expr->opno), GPOS_NEW(m_mp) CWStringConst(op_name->GetBuffer()), type);

	// create the DXL node holding the scalar opexpr
	CDXLNode *dxlnode = GPOS_NEW(m_mp) CDXLNode(m_mp, dxlop);

	// add children in the right order
	dxlnode->AddChild(left_node);
	dxlnode->AddChild(right_node);

	return dxlnode;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateConstToDXL
//
//	@doc:
//		Create a DXL node for a scalar const value from a GPDB Const
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::TranslateConstToDXL
	(
	const Expr *expr,
	const CMappingVarColId * // var_colid_mapping
	)
{
	GPOS_ASSERT(IsA(expr, Const));
	const Const *constant = (Const *) expr;

	CDXLNode *dxlnode = GPOS_NEW(m_mp) CDXLNode
									(
									m_mp,
									GPOS_NEW(m_mp) CDXLScalarConstValue
												(
												m_mp,
												TranslateConstToDXL(constant)
												)
									);

	return dxlnode;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateConstToDXL
//
//	@doc:
//		Create a DXL datum from a GPDB Const
//---------------------------------------------------------------------------
CDXLDatum *
CTranslatorScalarToDXL::TranslateConstToDXL
	(
	CMemoryPool *mp,
	CMDAccessor *mda,
	const Const *constant
	)
{
	CMDIdGPDB *mdid = GPOS_NEW(mp) CMDIdGPDB(constant->consttype);
	const IMDType *md_type= mda->RetrieveType(mdid);
	mdid->Release();

 	// translate gpdb datum into a DXL datum
	CDXLDatum *datum_dxl = CTranslatorScalarToDXL::TranslateDatumToDXL(mp, md_type, constant->consttypmod, constant->constisnull,
															 constant->constlen,
															 constant->constvalue);

	return datum_dxl;
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateConstToDXL
//
//	@doc:
//		Create a DXL datum from a GPDB Const
//---------------------------------------------------------------------------
CDXLDatum *
CTranslatorScalarToDXL::TranslateConstToDXL
	(
	const Const *constant
	)
	const
{
	return TranslateConstToDXL(m_mp, m_md_accessor, constant);
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateBoolExprToDXL
//
//	@doc:
//		Create a DXL node for a scalar boolean expression from a GPDB OpExpr.
//		This function can be used for constructing a scalar boolexpr operator appearing in a
//		base node (e.g. a scan node), or in an intermediate plan nodes.
//		The function allocates memory in the translator memory pool, and the caller
//		is responsible for freeing it
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::TranslateBoolExprToDXL
	(
	const Expr *expr,
	const CMappingVarColId* var_colid_mapping
	)
{
	GPOS_ASSERT(IsA(expr, BoolExpr));
	const BoolExpr *bool_expr = (BoolExpr *) expr;
	GPOS_ASSERT(0 < gpdb::ListLength(bool_expr->args));

	EdxlBoolExprType type = EdxlbooltypeFromGPDBBoolType(bool_expr->boolop);
	GPOS_ASSERT(EdxlBoolExprTypeSentinel != type);

	// create the DXL node holding the scalar boolean operator
	CDXLNode *dxlnode = GPOS_NEW(m_mp) CDXLNode(m_mp, GPOS_NEW(m_mp) CDXLScalarBoolExpr(m_mp, type));

	ULONG count = gpdb::ListLength(bool_expr->args);

	if ((NOT_EXPR != bool_expr->boolop) && (2 > count))
	{
		GPOS_RAISE
			(
			gpdxl::ExmaDXL,
			gpdxl::ExmiPlStmt2DXLConversion,
			GPOS_WSZ_LIT("Boolean Expression (OR / AND): Incorrect Number of Children ")
			);
	}
	else if ((NOT_EXPR == bool_expr->boolop) && (1 != count))
	{
		GPOS_RAISE
			(
			gpdxl::ExmaDXL,
			gpdxl::ExmiPlStmt2DXLConversion,
			GPOS_WSZ_LIT("Boolean Expression (NOT): Incorrect Number of Children ")
			);
	}

	TranslateScalarChildren(dxlnode, bool_expr->args, var_colid_mapping);

	return dxlnode;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateBooleanTestToDXL
//
//	@doc:
//		Create a DXL node for a scalar boolean test from a GPDB OpExpr.
//		This function can be used for constructing a scalar boolexpr operator appearing in a
//		base node (e.g. a scan node), or in an intermediate plan nodes.
//		The function allocates memory in the translator memory pool, and the caller
//		is responsible for freeing it
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::TranslateBooleanTestToDXL
	(
	const Expr *expr,
	const CMappingVarColId* var_colid_mapping
	)
{
	GPOS_ASSERT(IsA(expr, BooleanTest));

	const BooleanTest *boolean_test = (BooleanTest *) expr;

	GPOS_ASSERT(NULL != boolean_test->arg);

	static ULONG mapping[][2] =
		{
		{IS_TRUE, EdxlbooleantestIsTrue},
		{IS_NOT_TRUE, EdxlbooleantestIsNotTrue},
		{IS_FALSE, EdxlbooleantestIsFalse},
		{IS_NOT_FALSE, EdxlbooleantestIsNotFalse},
		{IS_UNKNOWN, EdxlbooleantestIsUnknown},
		{IS_NOT_UNKNOWN, EdxlbooleantestIsNotUnknown},
		};

	EdxlBooleanTestType type = EdxlbooleantestSentinel;
	const ULONG arity = GPOS_ARRAY_SIZE(mapping);
	for (ULONG ul = 0; ul < arity; ul++)
	{
		ULONG *elem = mapping[ul];
		if ((ULONG) boolean_test->booltesttype == elem[0])
		{
			type = (EdxlBooleanTestType) elem[1];
			break;
		}
	}
	GPOS_ASSERT(EdxlbooleantestSentinel != type && "Invalid boolean test type");

	// create the DXL node holding the scalar boolean test operator
	CDXLNode *dxlnode = GPOS_NEW(m_mp) CDXLNode
									(
									m_mp,
									GPOS_NEW(m_mp) CDXLScalarBooleanTest(m_mp,type)
									);

	CDXLNode *dxlnode_arg = TranslateScalarToDXL(boolean_test->arg, var_colid_mapping);
	GPOS_ASSERT(NULL != dxlnode_arg);

	dxlnode->AddChild(dxlnode_arg);

	return dxlnode;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateNullTestToDXL
//
//	@doc:
//		Create a DXL node for a scalar nulltest expression from a GPDB OpExpr
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::TranslateNullTestToDXL
	(
	const Expr *expr,
	const CMappingVarColId* var_colid_mapping
	)
{
	GPOS_ASSERT(IsA(expr, NullTest));
	const NullTest *null_test = (NullTest *) expr;

	GPOS_ASSERT(NULL != null_test->arg);
	CDXLNode *child_node = TranslateScalarToDXL(null_test->arg, var_colid_mapping);

	GPOS_ASSERT(NULL != child_node);
	GPOS_ASSERT(IS_NULL == null_test->nulltesttype || IS_NOT_NULL == null_test->nulltesttype);

	BOOL is_null = false;
	if (IS_NULL == null_test->nulltesttype)
	{
		is_null = true;
	}

	// create the DXL node holding the scalar NullTest operator
	CDXLNode *dxlnode = GPOS_NEW(m_mp) CDXLNode(m_mp, GPOS_NEW(m_mp) CDXLScalarNullTest(m_mp, is_null));
	dxlnode->AddChild(child_node);

	return dxlnode;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateCoalesceExprToDXL
//
//	@doc:
//		Create a DXL node for a coalesce function from a GPDB OpExpr
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::TranslateCoalesceExprToDXL
	(
	const Expr *expr,
	const CMappingVarColId* var_colid_mapping
	)
{
	GPOS_ASSERT(IsA(expr, CoalesceExpr));

	CoalesceExpr *coalesce_expr = (CoalesceExpr *) expr;
	GPOS_ASSERT(NULL != coalesce_expr->args);

	CDXLScalarCoalesce *dxlop = GPOS_NEW(m_mp) CDXLScalarCoalesce
											(
											m_mp,
											GPOS_NEW(m_mp) CMDIdGPDB(coalesce_expr->coalescetype)
											);

	CDXLNode *dxlnode = GPOS_NEW(m_mp) CDXLNode(m_mp, dxlop);

	TranslateScalarChildren(dxlnode, coalesce_expr->args, var_colid_mapping);

	return dxlnode;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateMinMaxExprToDXL
//
//	@doc:
//		Create a DXL node for a min/max operator from a GPDB OpExpr
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::TranslateMinMaxExprToDXL
	(
	const Expr *expr,
	const CMappingVarColId* var_colid_mapping
	)
{
	GPOS_ASSERT(IsA(expr, MinMaxExpr));

	MinMaxExpr *min_max_expr = (MinMaxExpr *) expr;
	GPOS_ASSERT(NULL != min_max_expr->args);

	CDXLScalarMinMax::EdxlMinMaxType min_max_type = CDXLScalarMinMax::EmmtSentinel;
	if (IS_GREATEST == min_max_expr->op)
	{
		min_max_type = CDXLScalarMinMax::EmmtMax;
	}
	else
	{
		GPOS_ASSERT(IS_LEAST == min_max_expr->op);
		min_max_type = CDXLScalarMinMax::EmmtMin;
	}

	CDXLScalarMinMax *dxlop = GPOS_NEW(m_mp) CDXLScalarMinMax
											(
											m_mp,
											GPOS_NEW(m_mp) CMDIdGPDB(min_max_expr->minmaxtype),
											min_max_type
											);

	CDXLNode *dxlnode = GPOS_NEW(m_mp) CDXLNode(m_mp, dxlop);

	TranslateScalarChildren(dxlnode, min_max_expr->args, var_colid_mapping);

	return dxlnode;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateScalarChildren
//
//	@doc:
//		Translate list elements and add them as children of the DXL node
//---------------------------------------------------------------------------
void
CTranslatorScalarToDXL::TranslateScalarChildren
	(
	CDXLNode *dxlnode,
	List *list,
	const CMappingVarColId* var_colid_mapping,
	BOOL *has_distributed_tables // output
	)
{
	ListCell *lc = NULL;
	ForEach (lc, list)
	{
		Expr *child_expr = (Expr *) lfirst(lc);
		CDXLNode *child_node = TranslateScalarToDXL(child_expr, var_colid_mapping, has_distributed_tables);
		GPOS_ASSERT(NULL != child_node);
		dxlnode->AddChild(child_node);
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateCaseExprToDXL
//
//	@doc:
//		Create a DXL node for a case statement from a GPDB OpExpr.
//		This function can be used for constructing a scalar opexpr operator appearing in a
//		base node (e.g. a scan node), or in an intermediate plan nodes.
//		The function allocates memory in the translator memory pool, and the caller
//		is responsible for freeing it
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::TranslateCaseExprToDXL
	(
	const Expr *expr,
	const CMappingVarColId* var_colid_mapping
	)
{
	GPOS_ASSERT(IsA(expr, CaseExpr));

	const CaseExpr *case_expr = (CaseExpr *) expr;

	if (NULL == case_expr->args)
	{
			GPOS_RAISE
				(
				gpdxl::ExmaDXL,
				gpdxl::ExmiPlStmt2DXLConversion,
				GPOS_WSZ_LIT("Do not support SIMPLE CASE STATEMENT")
				);
			return NULL;
	}

	if (NULL == case_expr->arg)
	{
		return CreateScalarIfStmtFromCaseExpr(case_expr, var_colid_mapping);
	}

	return CreateScalarSwitchFromCaseExpr(case_expr, var_colid_mapping);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::CreateScalarSwitchFromCaseExpr
//
//	@doc:
//		Create a DXL Switch node from a GPDB CaseExpr.
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::CreateScalarSwitchFromCaseExpr
	(
	const CaseExpr *case_expr,
	const CMappingVarColId* var_colid_mapping
	)
{
	GPOS_ASSERT (NULL != case_expr->arg);

	CDXLScalarSwitch *dxlop = GPOS_NEW(m_mp) CDXLScalarSwitch
												(
												m_mp,
												GPOS_NEW(m_mp) CMDIdGPDB(case_expr->casetype)
												);
	CDXLNode *switch_node = GPOS_NEW(m_mp) CDXLNode(m_mp, dxlop);

	// translate the switch expression
	CDXLNode *dxlnode_arg = TranslateScalarToDXL(case_expr->arg, var_colid_mapping);
	switch_node->AddChild(dxlnode_arg);

	// translate the cases
	ListCell *lc = NULL;
	ForEach (lc, case_expr->args)
	{
		CaseWhen *expr = (CaseWhen *) lfirst(lc);

		CDXLScalarSwitchCase *swtich_case = GPOS_NEW(m_mp) CDXLScalarSwitchCase(m_mp);
		CDXLNode *switch_case_node = GPOS_NEW(m_mp) CDXLNode(m_mp, swtich_case);

		CDXLNode *cmp_expr_node = TranslateScalarToDXL(expr->expr, var_colid_mapping);
		GPOS_ASSERT(NULL != cmp_expr_node);

		CDXLNode *result_node = TranslateScalarToDXL(expr->result, var_colid_mapping);
		GPOS_ASSERT(NULL != result_node);

		switch_case_node->AddChild(cmp_expr_node);
		switch_case_node->AddChild(result_node);

		// add current case to switch node
		switch_node->AddChild(switch_case_node);
	}

	// translate the "else" clause
	if (NULL != case_expr->defresult)
	{
		CDXLNode *default_result_node = TranslateScalarToDXL(case_expr->defresult, var_colid_mapping);
		GPOS_ASSERT(NULL != default_result_node);

		switch_node->AddChild(default_result_node);

	}

	return switch_node;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateCaseTestExprToDXL
//
//	@doc:
//		Create a DXL node for a case test from a GPDB Expr
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::TranslateCaseTestExprToDXL
	(
	const Expr *expr,
	const CMappingVarColId* //var_colid_mapping
	)
{
	GPOS_ASSERT(IsA(expr, CaseTestExpr));
	const CaseTestExpr *case_test_expr = (CaseTestExpr *) expr;
	CDXLScalarCaseTest *dxlop = GPOS_NEW(m_mp) CDXLScalarCaseTest
												(
												m_mp,
												GPOS_NEW(m_mp) CMDIdGPDB(case_test_expr->typeId)
												);

	return GPOS_NEW(m_mp) CDXLNode(m_mp, dxlop);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::CreateScalarIfStmtFromCaseExpr
//
//	@doc:
//		Create a DXL If node from a GPDB CaseExpr
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::CreateScalarIfStmtFromCaseExpr
	(
	const CaseExpr *case_expr,
	const CMappingVarColId* var_colid_mapping
	)
{
	GPOS_ASSERT (NULL == case_expr->arg);
	const ULONG when_clause_count = gpdb::ListLength(case_expr->args);

	CDXLNode *root_if_tree_node = NULL;
	CDXLNode *cur_node = NULL;

	for (ULONG ul = 0; ul < when_clause_count; ul++)
	{
		CDXLScalarIfStmt *if_stmt_new_dxl = GPOS_NEW(m_mp) CDXLScalarIfStmt
															(
															m_mp,
															GPOS_NEW(m_mp) CMDIdGPDB(case_expr->casetype)
															);

		CDXLNode *if_stmt_new_node = GPOS_NEW(m_mp) CDXLNode(m_mp, if_stmt_new_dxl);

		CaseWhen *expr = (CaseWhen *) gpdb::ListNth(case_expr->args, ul);
		GPOS_ASSERT(IsA(expr, CaseWhen));

		CDXLNode *cond_node = TranslateScalarToDXL(expr->expr, var_colid_mapping);
		CDXLNode *result_node = TranslateScalarToDXL(expr->result, var_colid_mapping);

		GPOS_ASSERT(NULL != cond_node);
		GPOS_ASSERT(NULL != result_node);

		if_stmt_new_node->AddChild(cond_node);
		if_stmt_new_node->AddChild(result_node);

		if(NULL == root_if_tree_node)
		{
			root_if_tree_node = if_stmt_new_node;
		}
		else
		{
			cur_node->AddChild(if_stmt_new_node);
		}
		cur_node = if_stmt_new_node;
	}

	if (NULL != case_expr->defresult)
	{
		CDXLNode *default_result_node = TranslateScalarToDXL(case_expr->defresult, var_colid_mapping);
		GPOS_ASSERT(NULL != default_result_node);
		cur_node->AddChild(default_result_node);
	}

	return root_if_tree_node;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateRelabelTypeToDXL
//
//	@doc:
//		Create a DXL node for a scalar RelabelType expression from a GPDB RelabelType
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::TranslateRelabelTypeToDXL
	(
	const Expr *expr,
	const CMappingVarColId* var_colid_mapping
	)
{
	GPOS_ASSERT(IsA(expr, RelabelType));

	const RelabelType *relabel_type = (RelabelType *) expr;

	GPOS_ASSERT(NULL != relabel_type->arg);

	CDXLNode *child_node = TranslateScalarToDXL(relabel_type->arg, var_colid_mapping);

	GPOS_ASSERT(NULL != child_node);

	// create the DXL node holding the scalar boolean operator
	CDXLNode *dxlnode = GPOS_NEW(m_mp) CDXLNode
									(
									m_mp,
									GPOS_NEW(m_mp) CDXLScalarCast
												(
												m_mp,
												GPOS_NEW(m_mp) CMDIdGPDB(relabel_type->resulttype),
												GPOS_NEW(m_mp) CMDIdGPDB(0) // casting function oid
												)
									);
	dxlnode->AddChild(child_node);

	return dxlnode;
}

//---------------------------------------------------------------------------
//      @function:
//              CTranslatorScalarToDXL::TranslateCoerceToDomainToDXL
//
//      @doc:
//              Create a DXL node for a scalar coerce expression from a
//             GPDB coerce expression
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::TranslateCoerceToDomainToDXL
        (
        const Expr *expr,
        const CMappingVarColId* var_colid_mapping
        )
{
        GPOS_ASSERT(IsA(expr, CoerceToDomain));

        const CoerceToDomain *coerce = (CoerceToDomain *) expr;

        GPOS_ASSERT(NULL != coerce->arg);

        CDXLNode *child_node = TranslateScalarToDXL(coerce->arg, var_colid_mapping);

        GPOS_ASSERT(NULL != child_node);

        // create the DXL node holding the scalar boolean operator
        CDXLNode *dxlnode = GPOS_NEW(m_mp) CDXLNode
                                                                        (
                                                                        m_mp,
                                                                        GPOS_NEW(m_mp) CDXLScalarCoerceToDomain
                                                                                                (
                                                                                                m_mp,
                                                                                                GPOS_NEW(m_mp) CMDIdGPDB(coerce->resulttype),
                                                                                               coerce->resulttypmod,
                                                                                               (EdxlCoercionForm) coerce->coercionformat,
                                                                                               coerce->location
                                                                                                )
                                                                        );
        dxlnode->AddChild(child_node);

        return dxlnode;
}

//---------------------------------------------------------------------------
//      @function:
//              CTranslatorScalarToDXL::TranslateCoerceViaIOToDXL
//
//      @doc:
//              Create a DXL node for a scalar coerce expression from a
//             GPDB coerce expression
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::TranslateCoerceViaIOToDXL
        (
        const Expr *expr,
        const CMappingVarColId* var_colid_mapping
        )
{
        GPOS_ASSERT(IsA(expr, CoerceViaIO));

        const CoerceViaIO *coerce = (CoerceViaIO *) expr;

        GPOS_ASSERT(NULL != coerce->arg);

        CDXLNode *child_node = TranslateScalarToDXL(coerce->arg, var_colid_mapping);

        GPOS_ASSERT(NULL != child_node);

        // create the DXL node holding the scalar boolean operator
        CDXLNode *dxlnode = GPOS_NEW(m_mp) CDXLNode
                                                                        (
                                                                        m_mp,
                                                                        GPOS_NEW(m_mp) CDXLScalarCoerceViaIO
                                                                                                (
                                                                                                m_mp,
                                                                                                GPOS_NEW(m_mp) CMDIdGPDB(coerce->resulttype),
                                                                                               -1,
                                                                                               (EdxlCoercionForm) coerce->coerceformat,
                                                                                               coerce->location
                                                                                                )
                                                                        );
        dxlnode->AddChild(child_node);

        return dxlnode;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateArrayCoerceExprToDXL
//	@doc:
//		Create a DXL node for a scalar array coerce expression from a
// 		GPDB Array Coerce expression
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::TranslateArrayCoerceExprToDXL
	(
	const Expr *expr,
	const CMappingVarColId* var_colid_mapping
	)
{
	GPOS_ASSERT(IsA(expr, ArrayCoerceExpr));
	const ArrayCoerceExpr *array_coerce_expr = (ArrayCoerceExpr *) expr;
	
	GPOS_ASSERT(NULL != array_coerce_expr->arg);
	
	CDXLNode *child_node = TranslateScalarToDXL(array_coerce_expr->arg, var_colid_mapping);
	
	GPOS_ASSERT(NULL != child_node);
	
	CDXLNode *dxlnode = GPOS_NEW(m_mp) CDXLNode
					(
					m_mp,
					GPOS_NEW(m_mp) CDXLScalarArrayCoerceExpr
							(
							m_mp,
							GPOS_NEW(m_mp) CMDIdGPDB(array_coerce_expr->elemfuncid),
							GPOS_NEW(m_mp) CMDIdGPDB(array_coerce_expr->resulttype),
							array_coerce_expr->resulttypmod,
							array_coerce_expr->isExplicit,
							(EdxlCoercionForm) array_coerce_expr->coerceformat,
							array_coerce_expr->location
							)
					);
	
        dxlnode->AddChild(child_node);

        return dxlnode;
}




//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateFuncExprToDXL
//
//	@doc:
//		Create a DXL node for a scalar funcexpr from a GPDB FuncExpr
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::TranslateFuncExprToDXL
	(
	const Expr *expr,
	const CMappingVarColId* var_colid_mapping
	)
{
	GPOS_ASSERT(IsA(expr, FuncExpr));
	const FuncExpr *func_expr = (FuncExpr *) expr;
	int32 type_modifier = gpdb::ExprTypeMod((Node *) expr);

	CMDIdGPDB *mdid_func = GPOS_NEW(m_mp) CMDIdGPDB(func_expr->funcid);

	// create the DXL node holding the scalar funcexpr
	CDXLNode *dxlnode = GPOS_NEW(m_mp) CDXLNode
									(
									m_mp,
									GPOS_NEW(m_mp) CDXLScalarFuncExpr
												(
												m_mp,
												mdid_func,
												GPOS_NEW(m_mp) CMDIdGPDB(func_expr->funcresulttype),
												type_modifier,
												func_expr->funcretset
												)
									);

	const IMDFunction *md_func = m_md_accessor->RetrieveFunc(mdid_func);
	if (IMDFunction::EfsVolatile == md_func->GetFuncStability())
	{
		ListCell *lc = NULL;
		ForEach (lc, func_expr->args)
		{
			Node *arg_node = (Node *) lfirst(lc);
			if (CTranslatorUtils::HasSubquery(arg_node))
			{
				GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature,
						GPOS_WSZ_LIT("Volatile functions with subqueries in arguments"));
			}
		}
	}

	TranslateScalarChildren(dxlnode, func_expr->args, var_colid_mapping);

	return dxlnode;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateAggrefToDXL
//
//	@doc:
//		Create a DXL node for a scalar aggref from a GPDB FuncExpr
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::TranslateAggrefToDXL
	(
	const Expr *expr,
	const CMappingVarColId* var_colid_mapping
	)
{
	GPOS_ASSERT(IsA(expr, Aggref));
	const Aggref *aggref = (Aggref *) expr;

	static ULONG mapping[][2] =
		{
		{AGGSTAGE_NORMAL, EdxlaggstageNormal},
		{AGGSTAGE_PARTIAL, EdxlaggstagePartial},
		{AGGSTAGE_INTERMEDIATE, EdxlaggstageIntermediate},
		{AGGSTAGE_FINAL, EdxlaggstageFinal},
		};

	EdxlAggrefStage agg_stage = EdxlaggstageSentinel;
	const ULONG arity = GPOS_ARRAY_SIZE(mapping);
	for (ULONG ul = 0; ul < arity; ul++)
	{
		ULONG *elem = mapping[ul];
		if ((ULONG) aggref->aggstage == elem[0])
		{
			agg_stage = (EdxlAggrefStage) elem[1];
			break;
		}
	}
	GPOS_ASSERT(EdxlaggstageSentinel != agg_stage && "Invalid agg stage");

	CMDIdGPDB *agg_mdid = GPOS_NEW(m_mp) CMDIdGPDB(aggref->aggfnoid);
	const IMDAggregate *md_agg = m_md_accessor->RetrieveAgg(agg_mdid);

	if (md_agg->IsOrdered())
	{
		GPOS_ASSERT_IMP(NULL == aggref->aggorder, md_agg->IsOrdered());
		GPOS_RAISE
			(
			gpdxl::ExmaDXL,
			gpdxl::ExmiPlStmt2DXLConversion,
			GPOS_WSZ_LIT("Ordered aggregates")
			);
	}

	if (0 != aggref->agglevelsup)
	{
		// TODO: Feb 05 2015, remove temporary fix to avoid erroring out during execution
		GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLError, GPOS_WSZ_LIT("Aggregate functions with outer references"));
	}

	IMDId *mdid_return_type = CScalarAggFunc::PmdidLookupReturnType(agg_mdid, (EdxlaggstageNormal == agg_stage), m_md_accessor);
	IMDId *resolved_ret_type = NULL;
	if (m_md_accessor->RetrieveType(mdid_return_type)->IsAmbiguous())
	{
		// if return type given by MD cache is ambiguous, use type provided by aggref node
		resolved_ret_type = GPOS_NEW(m_mp) CMDIdGPDB(aggref->aggtype);
	}

	CDXLScalarAggref *aggref_scalar = GPOS_NEW(m_mp) CDXLScalarAggref(m_mp,agg_mdid, resolved_ret_type, aggref->aggdistinct, agg_stage);

	// create the DXL node holding the scalar aggref
	CDXLNode *dxlnode = GPOS_NEW(m_mp) CDXLNode(m_mp, aggref_scalar);

	TranslateScalarChildren(dxlnode, aggref->args, var_colid_mapping);

	return dxlnode;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::Edxlfb
//
//	@doc:
//		Return the DXL representation of window frame boundary
//---------------------------------------------------------------------------
EdxlFrameBoundary
CTranslatorScalarToDXL::Edxlfb
	(
	WindowBoundingKind kind,
	Node *pnode
	)
	const
{
	static ULONG rgrgulMapping[][2] =
			{
			{WINDOW_UNBOUND_PRECEDING, EdxlfbUnboundedPreceding},
			{WINDOW_BOUND_PRECEDING, EdxlfbBoundedPreceding},
			{WINDOW_CURRENT_ROW, EdxlfbCurrentRow},
			{WINDOW_BOUND_FOLLOWING, EdxlfbBoundedFollowing},
			{WINDOW_UNBOUND_FOLLOWING, EdxlfbUnboundedFollowing},
			{WINDOW_DELAYED_BOUND_PRECEDING, EdxlfbDelayedBoundedPreceding},
		    {WINDOW_DELAYED_BOUND_FOLLOWING, EdxlfbDelayedBoundedFollowing}
			};

	const ULONG ulArity = GPOS_ARRAY_SIZE(rgrgulMapping);
	EdxlFrameBoundary edxlfb = EdxlfbSentinel;
	for (ULONG ul = 0; ul < ulArity; ul++)
	{
		ULONG *pulElem = rgrgulMapping[ul];
		if ((ULONG) kind == pulElem[0])
		{
			edxlfb = (EdxlFrameBoundary) pulElem[1];

			if ((WINDOW_BOUND_FOLLOWING == kind) && ((NULL == pnode) || !IsA(pnode, Const)))
			{
				edxlfb = EdxlfbDelayedBoundedFollowing;
			}

			if ((WINDOW_BOUND_PRECEDING == kind) && ((NULL == pnode) || !IsA(pnode, Const)))
			{
				edxlfb = EdxlfbDelayedBoundedPreceding;
			}

			break;
		}
	}
	GPOS_ASSERT(EdxlfbSentinel != edxlfb && "Invalid window frame boundary");

	return edxlfb;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::GetWindowFrame
//
//	@doc:
//		Create a DXL window frame from a GPDB WindowFrame
//---------------------------------------------------------------------------
CDXLWindowFrame *
CTranslatorScalarToDXL::GetWindowFrame
	(
	const Expr *expr,
	const CMappingVarColId* var_colid_mapping,
	CDXLNode *new_scalar_proj_list,
	BOOL *has_distributed_tables // output
	)
{
	GPOS_ASSERT(IsA(expr, WindowFrame));
	const WindowFrame *pwindowframe = (WindowFrame *) expr;

	EdxlFrameSpec frame_spec = EdxlfsRow;
	if (!pwindowframe->is_rows)
	{
		frame_spec = EdxlfsRange;
	}

	EdxlFrameBoundary leading_boundary = Edxlfb(pwindowframe->lead->kind, pwindowframe->lead->val);
	EdxlFrameBoundary trailing_boundary = Edxlfb(pwindowframe->trail->kind, pwindowframe->trail->val);

	static ULONG rgrgulExclusionMapping[][2] =
			{
			{WINDOW_EXCLUSION_NULL, EdxlfesNulls},
			{WINDOW_EXCLUSION_CUR_ROW, EdxlfesCurrentRow},
			{WINDOW_EXCLUSION_GROUP, EdxlfesGroup},
			{WINDOW_EXCLUSION_TIES, EdxlfesTies},
			{WINDOW_EXCLUSION_NO_OTHERS, EdxlfesNone},
			};

	const ULONG ulArityExclusion = GPOS_ARRAY_SIZE(rgrgulExclusionMapping);
	EdxlFrameExclusionStrategy strategy = EdxlfesSentinel;
	for (ULONG ul = 0; ul < ulArityExclusion; ul++)
	{
		ULONG *pulElem = rgrgulExclusionMapping[ul];
		if ((ULONG) pwindowframe->exclude == pulElem[0])
		{
			strategy = (EdxlFrameExclusionStrategy) pulElem[1];
			break;
		}
	}
	GPOS_ASSERT(EdxlfesSentinel != strategy && "Invalid window frame exclusion");

	CDXLNode *lead_edge = GPOS_NEW(m_mp) CDXLNode(m_mp, GPOS_NEW(m_mp) CDXLScalarWindowFrameEdge(m_mp, true /* fLeading */, leading_boundary));
	CDXLNode *trail_edge = GPOS_NEW(m_mp) CDXLNode(m_mp, GPOS_NEW(m_mp) CDXLScalarWindowFrameEdge(m_mp, false /* fLeading */, trailing_boundary));

	// translate the lead and trail value
	if (NULL != pwindowframe->lead->val)
	{
		lead_edge->AddChild(TranslateWindowFrameEdgeToDXL(pwindowframe->lead->val, var_colid_mapping, new_scalar_proj_list, has_distributed_tables));
	}

	if (NULL != pwindowframe->trail->val)
	{
		trail_edge->AddChild(TranslateWindowFrameEdgeToDXL(pwindowframe->trail->val, var_colid_mapping, new_scalar_proj_list, has_distributed_tables));
	}

	CDXLWindowFrame *window_frame_dxl = GPOS_NEW(m_mp) CDXLWindowFrame(m_mp, frame_spec, strategy, lead_edge, trail_edge);

	return window_frame_dxl;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateWindowFrameEdgeToDXL
//
//	@doc:
//		Translate the window frame edge, if the column used in the edge is a
// 		computed column then add it to the project list
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::TranslateWindowFrameEdgeToDXL
	(
	const Node *node,
	const CMappingVarColId* var_colid_mapping,
	CDXLNode *new_scalar_proj_list,
	BOOL *has_distributed_tables
	)
{
	CDXLNode *val_node = TranslateScalarToDXL((Expr *) node, var_colid_mapping, has_distributed_tables);

	if (m_is_query_mode && !IsA(node, Var) && !IsA(node, Const))
	{
		GPOS_ASSERT(NULL != new_scalar_proj_list);
		CWStringConst unnamed_col(GPOS_WSZ_LIT("?column?"));
		CMDName *alias_mdname = GPOS_NEW(m_mp) CMDName(m_mp, &unnamed_col);
		ULONG project_element_id = m_colid_generator->next_id();

		// construct a projection element
		CDXLNode *project_element_node = GPOS_NEW(m_mp) CDXLNode(m_mp, GPOS_NEW(m_mp) CDXLScalarProjElem(m_mp, project_element_id, alias_mdname));
		project_element_node->AddChild(val_node);

		// add it to the computed columns project list
		new_scalar_proj_list->AddChild(project_element_node);

		// construct a new scalar ident
		CDXLScalarIdent *scalar_ident = GPOS_NEW(m_mp) CDXLScalarIdent
													(
													m_mp,
													GPOS_NEW(m_mp) CDXLColRef
																(
																m_mp,
																GPOS_NEW(m_mp) CMDName(m_mp, &unnamed_col),
																project_element_id,
																GPOS_NEW(m_mp) CMDIdGPDB(gpdb::ExprType(const_cast<Node*>(node))),
																gpdb::ExprTypeMod(const_cast<Node*>(node))
																)
													);

		val_node = GPOS_NEW(m_mp) CDXLNode(m_mp, scalar_ident);
	}

	return val_node;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateWindowRefToDXL
//
//	@doc:
//		Create a DXL node for a scalar window ref from a GPDB WindowRef
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::TranslateWindowRefToDXL
	(
	const Expr *expr,
	const CMappingVarColId* var_colid_mapping
	)
{
	GPOS_ASSERT(IsA(expr, WindowRef));

	const WindowRef *pwindowref = (WindowRef *) expr;

	static ULONG mapping[][2] =
		{
		{WINSTAGE_IMMEDIATE, EdxlwinstageImmediate},
		{WINSTAGE_PRELIMINARY, EdxlwinstagePreliminary},
		{WINSTAGE_ROWKEY, EdxlwinstageRowKey},
		};

	const ULONG arity = GPOS_ARRAY_SIZE(mapping);
	EdxlWinStage dxl_win_stage = EdxlwinstageSentinel;

	for (ULONG ul = 0; ul < arity; ul++)
	{
		ULONG *pulElem = mapping[ul];
		if ((ULONG) pwindowref->winstage == pulElem[0])
		{
			dxl_win_stage = (EdxlWinStage) pulElem[1];
			break;
		}
	}

	ULONG win_spec_pos = (ULONG) pwindowref->winlevel;
	if (m_is_query_mode)
	{
		win_spec_pos = (ULONG) pwindowref->winspec;
	}

	GPOS_ASSERT(EdxlwinstageSentinel != dxl_win_stage && "Invalid window stage");

	// fallback if window function is not supported
	if (WINDOW_PERCENT_RANK == pwindowref->winfnoid)
	{
		GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, GPOS_WSZ_LIT("PERCENT_RANK Window Functions"));
	}
	else if (CUME_DIST_OID == pwindowref->winfnoid)
	{
		GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, GPOS_WSZ_LIT("CUME_DIST Window Functions"));
	}
	else if (WINDOW_NTILE_INT4 == pwindowref->winfnoid ||
			 WINDOW_NTILE_INT8 == pwindowref->winfnoid ||
			 WINDOW_NTILE_NUMERIC == pwindowref->winfnoid)
	{
		GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, GPOS_WSZ_LIT("NTILE Window Functions"));
	}

	CDXLScalarWindowRef *winref_dxlop = GPOS_NEW(m_mp) CDXLScalarWindowRef
													(
													m_mp,
													GPOS_NEW(m_mp) CMDIdGPDB(pwindowref->winfnoid),
													GPOS_NEW(m_mp) CMDIdGPDB(pwindowref->restype),
													pwindowref->windistinct,
													false,
													false,
													dxl_win_stage,
													win_spec_pos
													);

	// create the DXL node holding the scalar aggref
	CDXLNode *dxlnode = GPOS_NEW(m_mp) CDXLNode(m_mp, winref_dxlop);

	TranslateScalarChildren(dxlnode, pwindowref->args, var_colid_mapping);

	return dxlnode;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::CreateScalarCondFromQual
//
//	@doc:
//		Create a DXL scalar boolean operator node from a GPDB qual list.
//		The function allocates memory in the translator memory pool, and the caller
//		is responsible for freeing it
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::CreateScalarCondFromQual
	(
	List *quals,
	const CMappingVarColId* var_colid_mapping,
	BOOL *has_distributed_tables
	)
{
	if (NULL == quals || 0 == gpdb::ListLength(quals))
	{
		return NULL;
	}

	if (1 == gpdb::ListLength(quals))
	{
		Expr *expr = (Expr *) gpdb::ListNth(quals, 0);
		return TranslateScalarToDXL(expr, var_colid_mapping, has_distributed_tables);
	}
	else
	{
		// GPDB assumes that if there are a list of qual conditions then it is an implicit AND operation
		// Here we build the left deep AND tree
		CDXLNode *dxlnode = GPOS_NEW(m_mp) CDXLNode(m_mp, GPOS_NEW(m_mp) CDXLScalarBoolExpr(m_mp, Edxland));

		TranslateScalarChildren(dxlnode, quals, var_colid_mapping, has_distributed_tables);

		return dxlnode;
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::CreateFilterFromQual
//
//	@doc:
//		Create a DXL scalar filter node from a GPDB qual list.
//		The function allocates memory in the translator memory pool, and the caller
//		is responsible for freeing it
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::CreateFilterFromQual
	(
	List *quals,
	const CMappingVarColId* var_colid_mapping,
	Edxlopid filter_type,
	BOOL *has_distributed_tables // output
	)
{
	CDXLScalarFilter *dxlop = NULL;

	switch (filter_type)
	{
		case EdxlopScalarFilter:
			dxlop = GPOS_NEW(m_mp) CDXLScalarFilter(m_mp);
			break;
		case EdxlopScalarJoinFilter:
			dxlop = GPOS_NEW(m_mp) CDXLScalarJoinFilter(m_mp);
			break;
		case EdxlopScalarOneTimeFilter:
			dxlop = GPOS_NEW(m_mp) CDXLScalarOneTimeFilter(m_mp);
			break;
		default:
			GPOS_ASSERT(!"Unrecognized filter type");
	}


	CDXLNode *filter_dxlnode = GPOS_NEW(m_mp) CDXLNode(m_mp, dxlop);

	CDXLNode *cond_node = CreateScalarCondFromQual(quals, var_colid_mapping, has_distributed_tables);

	if (NULL != cond_node)
	{
		filter_dxlnode->AddChild(cond_node);
	}

	return filter_dxlnode;
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateSubLinkToDXL
//
//	@doc:
//		Create a DXL node from a GPDB sublink node.
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::TranslateSubLinkToDXL
	(
	const Expr *expr,
	const CMappingVarColId* var_colid_mapping
	)
{
	const SubLink *sublink = (SubLink *) expr;

	switch (sublink->subLinkType)
	{
		case EXPR_SUBLINK:
			return CreateScalarSubqueryFromSublink(sublink, var_colid_mapping);

		case ALL_SUBLINK:
		case ANY_SUBLINK:
			return CreateQuantifiedSubqueryFromSublink(sublink, var_colid_mapping);

		case EXISTS_SUBLINK:
			return CreateExistSubqueryFromSublink(sublink, var_colid_mapping);

		default:
			{
				GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, GPOS_WSZ_LIT("Non-Scalar Subquery"));
				return NULL;
			}

	}
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::CreateQuantifiedSubqueryFromSublink
//
//	@doc:
//		Create ANY / ALL quantified sub query from a GPDB sublink node
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::CreateQuantifiedSubqueryFromSublink
	(
	const SubLink *sublink,
	const CMappingVarColId* var_colid_mapping
	)
{
	CMappingVarColId *var_colid_map_copy = var_colid_mapping->CopyMapColId(m_mp);

	CAutoP<CTranslatorQueryToDXL> query_to_dxl_translator;
	query_to_dxl_translator = CTranslatorQueryToDXL::QueryToDXLInstance
							(
							m_mp,
							m_md_accessor,
							m_colid_generator,
							m_cte_id_generator,
							var_colid_map_copy,
							(Query *) sublink->subselect,
							m_query_level + 1,
							m_cte_entries
							);

	CDXLNode *inner_dxlnode = query_to_dxl_translator->TranslateSelectQueryToDXL();

	CDXLNodeArray *query_output_dxlnode_array = query_to_dxl_translator->GetQueryOutputCols();
	CDXLNodeArray *cte_dxlnode_array = query_to_dxl_translator->GetCTEs();
	CUtils::AddRefAppend(m_cte_producers, cte_dxlnode_array);

	if (1 != query_output_dxlnode_array->Size())
	{
		GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, GPOS_WSZ_LIT("Non-Scalar Subquery"));
	}

	m_has_distributed_tables = m_has_distributed_tables || query_to_dxl_translator->HasDistributedTables();

	CDXLNode *dxl_sc_ident = (*query_output_dxlnode_array)[0];
	GPOS_ASSERT(NULL != dxl_sc_ident);

	// get dxl scalar identifier
	CDXLScalarIdent *scalar_ident = dynamic_cast<CDXLScalarIdent *>(dxl_sc_ident->GetOperator());

	// get the dxl column reference
	const CDXLColRef *dxl_colref = scalar_ident->GetDXLColRef();
	const ULONG colid = dxl_colref->Id();

	// get the test expression
	GPOS_ASSERT(IsA(sublink->testexpr, OpExpr));
	OpExpr *op_expr = (OpExpr*) sublink->testexpr;

	IMDId *mdid = GPOS_NEW(m_mp) CMDIdGPDB(op_expr->opno);

	// get operator name
	const CWStringConst *str = GetDXLArrayCmpType(mdid);

	// translate left hand side of the expression
	GPOS_ASSERT(NULL != op_expr->args);
	Expr* LHS_expr = (Expr*) gpdb::ListNth(op_expr->args, 0);

	CDXLNode *outer_dxlnode = TranslateScalarToDXL(LHS_expr, var_colid_mapping);

	CDXLNode *dxlnode = NULL;
	CDXLScalar *subquery = NULL;

	GPOS_ASSERT(ALL_SUBLINK == sublink->subLinkType || ANY_SUBLINK == sublink->subLinkType);
	if (ALL_SUBLINK == sublink->subLinkType)
	{
		subquery = GPOS_NEW(m_mp) CDXLScalarSubqueryAll
								(
								m_mp,
								mdid,
								GPOS_NEW(m_mp) CMDName(m_mp, str),
								colid
								);

	}
	else
	{
		subquery = GPOS_NEW(m_mp) CDXLScalarSubqueryAny
								(
								m_mp,
								mdid,
								GPOS_NEW(m_mp) CMDName(m_mp, str),
								colid
								);
	}

	dxlnode = GPOS_NEW(m_mp) CDXLNode(m_mp, subquery);

	dxlnode->AddChild(outer_dxlnode);
	dxlnode->AddChild(inner_dxlnode);

#ifdef GPOS_DEBUG
	dxlnode->GetOperator()->AssertValid(dxlnode, false /* fValidateChildren */);
#endif

	return dxlnode;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::CreateScalarSubqueryFromSublink
//
//	@doc:
//		Create a scalar subquery from a GPDB sublink node
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::CreateScalarSubqueryFromSublink
	(
	const SubLink *sublink,
	const CMappingVarColId *var_colid_mapping
	)
{
	CMappingVarColId *var_colid_map_copy = var_colid_mapping->CopyMapColId(m_mp);

	Query *subselect = (Query *) sublink->subselect;
	CAutoP<CTranslatorQueryToDXL> query_to_dxl_translator;
	query_to_dxl_translator = CTranslatorQueryToDXL::QueryToDXLInstance
							(
							m_mp,
							m_md_accessor,
							m_colid_generator,
							m_cte_id_generator,
							var_colid_map_copy,
							subselect,
							m_query_level + 1,
							m_cte_entries
							);
	CDXLNode *subquery_dxlnode = query_to_dxl_translator->TranslateSelectQueryToDXL();

	CDXLNodeArray *query_output_dxlnode_array = query_to_dxl_translator->GetQueryOutputCols();

	GPOS_ASSERT(1 == query_output_dxlnode_array->Size());

	CDXLNodeArray *cte_dxlnode_array = query_to_dxl_translator->GetCTEs();
	CUtils::AddRefAppend(m_cte_producers, cte_dxlnode_array);
	m_has_distributed_tables = m_has_distributed_tables || query_to_dxl_translator->HasDistributedTables();

	// get dxl scalar identifier
	CDXLNode *dxl_sc_ident = (*query_output_dxlnode_array)[0];
	GPOS_ASSERT(NULL != dxl_sc_ident);

	CDXLScalarIdent *scalar_ident = CDXLScalarIdent::Cast(dxl_sc_ident->GetOperator());

	// get the dxl column reference
	const CDXLColRef *dxl_colref = scalar_ident->GetDXLColRef();
	const ULONG colid = dxl_colref->Id();

	CDXLNode *dxlnode = GPOS_NEW(m_mp) CDXLNode(m_mp, GPOS_NEW(m_mp) CDXLScalarSubquery(m_mp, colid));

	dxlnode->AddChild(subquery_dxlnode);

	return dxlnode;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateArrayExprToDXL
//
//	@doc:
//		Translate array
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::TranslateArrayExprToDXL
	(
	const Expr *expr,
	const CMappingVarColId* var_colid_mapping
	)
{
	GPOS_ASSERT(IsA(expr, ArrayExpr));

	const ArrayExpr *parrayexpr = (ArrayExpr *) expr;

	CDXLScalarArray *dxlop =
			GPOS_NEW(m_mp) CDXLScalarArray
						(
						m_mp,
						GPOS_NEW(m_mp) CMDIdGPDB(parrayexpr->element_typeid),
						GPOS_NEW(m_mp) CMDIdGPDB(parrayexpr->array_typeid),
						parrayexpr->multidims
						);

	CDXLNode *dxlnode = GPOS_NEW(m_mp) CDXLNode(m_mp, dxlop);

	TranslateScalarChildren(dxlnode, parrayexpr->elements, var_colid_mapping);

	return dxlnode;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateArrayRefToDXL
//
//	@doc:
//		Translate arrayref
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::TranslateArrayRefToDXL
	(
	const Expr *expr,
	const CMappingVarColId* var_colid_mapping
	)
{
	GPOS_ASSERT(IsA(expr, ArrayRef));

	const ArrayRef *parrayref = (ArrayRef *) expr;
	Oid restype;

	INT type_modifier = parrayref->reftypmod;
	/* slice and/or store operations yield the array type */
	if (parrayref->reflowerindexpr || parrayref->refassgnexpr)
		restype = parrayref->refarraytype;
	else
		restype = parrayref->refelemtype;

	CDXLScalarArrayRef *dxlop =
			GPOS_NEW(m_mp) CDXLScalarArrayRef
						(
						m_mp,
						GPOS_NEW(m_mp) CMDIdGPDB(parrayref->refelemtype),
						type_modifier,
						GPOS_NEW(m_mp) CMDIdGPDB(parrayref->refarraytype),
						GPOS_NEW(m_mp) CMDIdGPDB(restype)
						);

	CDXLNode *dxlnode = GPOS_NEW(m_mp) CDXLNode(m_mp, dxlop);

	// add children
	AddArrayIndexList(dxlnode, parrayref->reflowerindexpr, CDXLScalarArrayRefIndexList::EilbLower, var_colid_mapping);
	AddArrayIndexList(dxlnode, parrayref->refupperindexpr, CDXLScalarArrayRefIndexList::EilbUpper, var_colid_mapping);

	dxlnode->AddChild(TranslateScalarToDXL(parrayref->refexpr, var_colid_mapping));

	if (NULL != parrayref->refassgnexpr)
	{
		dxlnode->AddChild(TranslateScalarToDXL(parrayref->refassgnexpr, var_colid_mapping));
	}

	return dxlnode;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::AddArrayIndexList
//
//	@doc:
//		Add an indexlist to the given DXL arrayref node
//
//---------------------------------------------------------------------------
void
CTranslatorScalarToDXL::AddArrayIndexList
	(
	CDXLNode *dxlnode,
	List *list,
	CDXLScalarArrayRefIndexList::EIndexListBound index_list_bound,
	const CMappingVarColId* var_colid_mapping
	)
{
	GPOS_ASSERT(NULL != dxlnode);
	GPOS_ASSERT(EdxlopScalarArrayRef == dxlnode->GetOperator()->GetDXLOperator());
	GPOS_ASSERT(CDXLScalarArrayRefIndexList::EilbSentinel > index_list_bound);

	CDXLNode *index_list_dxlnode =
			GPOS_NEW(m_mp) CDXLNode
					(
					m_mp,
					GPOS_NEW(m_mp) CDXLScalarArrayRefIndexList(m_mp, index_list_bound)
					);

	TranslateScalarChildren(index_list_dxlnode, list, var_colid_mapping);
	dxlnode->AddChild(index_list_dxlnode);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::GetDXLArrayCmpType
//
//	@doc:
//		Get the operator name
//
//---------------------------------------------------------------------------
const CWStringConst *
CTranslatorScalarToDXL::GetDXLArrayCmpType
	(
	IMDId *mdid
	)
	const
{
	// get operator name
	const IMDScalarOp *md_scalar_op = m_md_accessor->RetrieveScOp(mdid);

	const CWStringConst *str = md_scalar_op->Mdname().GetMDName();

	return str;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::CreateExistSubqueryFromSublink
//
//	@doc:
//		Create a DXL EXISTS subquery node from the respective GPDB
//		sublink node
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::CreateExistSubqueryFromSublink
	(
	const SubLink *sublink,
	const CMappingVarColId* var_colid_mapping
	)
{
	GPOS_ASSERT(NULL != sublink);
	CMappingVarColId *var_colid_map_copy = var_colid_mapping->CopyMapColId(m_mp);

	CAutoP<CTranslatorQueryToDXL> query_to_dxl_translator;
	query_to_dxl_translator = CTranslatorQueryToDXL::QueryToDXLInstance
							(
							m_mp,
							m_md_accessor,
							m_colid_generator,
							m_cte_id_generator,
							var_colid_map_copy,
							(Query *) sublink->subselect,
							m_query_level + 1,
							m_cte_entries
							);
	CDXLNode *root_dxlnode = query_to_dxl_translator->TranslateSelectQueryToDXL();
	
	CDXLNodeArray *cte_dxlnode_array = query_to_dxl_translator->GetCTEs();
	CUtils::AddRefAppend(m_cte_producers, cte_dxlnode_array);
	m_has_distributed_tables = m_has_distributed_tables || query_to_dxl_translator->HasDistributedTables();
	
	CDXLNode *dxlnode = GPOS_NEW(m_mp) CDXLNode(m_mp, GPOS_NEW(m_mp) CDXLScalarSubqueryExists(m_mp));
	dxlnode->AddChild(root_dxlnode);

	return dxlnode;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateDatumToDXL
//
//	@doc:
//		Create CDXLDatum from GPDB datum
//---------------------------------------------------------------------------
CDXLDatum *
CTranslatorScalarToDXL::TranslateDatumToDXL
	(
	CMemoryPool *mp,
	const IMDType *md_type,
	INT type_modifier,
	BOOL is_null,
	ULONG len,
	Datum datum
	)
{
	static const SDXLDatumTranslatorElem translators[] =
		{
			{IMDType::EtiInt2  , &CTranslatorScalarToDXL::TranslateInt2DatumToDXL},
			{IMDType::EtiInt4  , &CTranslatorScalarToDXL::TranslateInt4DatumToDXL},
			{IMDType::EtiInt8 , &CTranslatorScalarToDXL::TranslateInt8DatumToDXL},
			{IMDType::EtiBool , &CTranslatorScalarToDXL::TranslateBoolDatumToDXL},
			{IMDType::EtiOid  , &CTranslatorScalarToDXL::TranslateOidDatumToDXL},
		};

	const ULONG num_translators = GPOS_ARRAY_SIZE(translators);
	// find translator for the datum type
	DxlDatumFromDatum *func_ptr = NULL;
	for (ULONG ul = 0; ul < num_translators; ul++)
	{
		SDXLDatumTranslatorElem elem = translators[ul];
		if (md_type->GetDatumType() == elem.type_info)
		{
			func_ptr = elem.func_ptr;
			break;
		}
	}

	if (NULL == func_ptr)
	{
		// generate a datum of generic type
		return TranslateGenericDatumToDXL(mp, md_type, type_modifier, is_null, len, datum);
	}
	else
	{
		return (*func_ptr)(mp, md_type, is_null, len, datum);
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateGenericDatumToDXL
//
//	@doc:
//		Translate a datum of generic type
//---------------------------------------------------------------------------
CDXLDatum *
CTranslatorScalarToDXL::TranslateGenericDatumToDXL
	(
	CMemoryPool *mp,
	const IMDType *md_type,
	INT type_modifier,
	BOOL is_null,
	ULONG len,
	Datum datum
	)
{
	CMDIdGPDB *mdid_old = CMDIdGPDB::CastMdid(md_type->MDId());
	CMDIdGPDB *mdid = GPOS_NEW(mp) CMDIdGPDB(*mdid_old);

	BOOL is_const_by_val = md_type->IsPassedByValue();
	BYTE *bytes = ExtractByteArrayFromDatum(mp, md_type, is_null, len, datum);
	ULONG length = 0;
	if (!is_null)
	{
		length = (ULONG) gpdb::DatumSize(datum, md_type->IsPassedByValue(), len);
	}

	CDouble double_value(0);
	if (CMDTypeGenericGPDB::HasByte2DoubleMapping(mdid))
	{
		double_value = ExtractDoubleValueFromDatum(mdid, is_null, bytes, datum);
	}

	LINT lint_value = 0;
	if (CMDTypeGenericGPDB::HasByte2IntMapping(mdid))
	{
		lint_value = ExtractLintValueFromDatum(mdid, is_null, bytes, length);
	}

	return CMDTypeGenericGPDB::CreateDXLDatumVal(mp, mdid, type_modifier, is_const_by_val, is_null, bytes, length, lint_value, double_value);
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateBoolDatumToDXL
//
//	@doc:
//		Translate a datum of type bool
//---------------------------------------------------------------------------
CDXLDatum *
CTranslatorScalarToDXL::TranslateBoolDatumToDXL
	(
	CMemoryPool *mp,
	const IMDType *md_type,
	BOOL is_null,
	ULONG , //len,
	Datum datum
	)
{
	GPOS_ASSERT(md_type->IsPassedByValue());
	CMDIdGPDB *mdid_old = CMDIdGPDB::CastMdid(md_type->MDId());
	CMDIdGPDB *mdid = GPOS_NEW(mp) CMDIdGPDB(*mdid_old);

	return GPOS_NEW(mp) CDXLDatumBool(mp, mdid, is_null, gpdb::BoolFromDatum(datum));
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateOidDatumToDXL
//
//	@doc:
//		Translate a datum of type oid
//---------------------------------------------------------------------------
CDXLDatum *
CTranslatorScalarToDXL::TranslateOidDatumToDXL
	(
	CMemoryPool *mp,
	const IMDType *md_type,
	BOOL is_null,
	ULONG , //len,
	Datum datum
	)
{
	GPOS_ASSERT(md_type->IsPassedByValue());
	CMDIdGPDB *mdid_old = CMDIdGPDB::CastMdid(md_type->MDId());
	CMDIdGPDB *mdid = GPOS_NEW(mp) CMDIdGPDB(*mdid_old);

	return GPOS_NEW(mp) CDXLDatumOid(mp, mdid, is_null, gpdb::OidFromDatum(datum));
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateInt2DatumToDXL
//
//	@doc:
//		Translate a datum of type int2
//---------------------------------------------------------------------------
CDXLDatum *
CTranslatorScalarToDXL::TranslateInt2DatumToDXL
	(
	CMemoryPool *mp,
	const IMDType *md_type,
	BOOL is_null,
	ULONG , //len,
	Datum datum
	)
{
	GPOS_ASSERT(md_type->IsPassedByValue());
	CMDIdGPDB *mdid_old = CMDIdGPDB::CastMdid(md_type->MDId());
	CMDIdGPDB *mdid = GPOS_NEW(mp) CMDIdGPDB(*mdid_old);

	return GPOS_NEW(mp) CDXLDatumInt2(mp, mdid, is_null, gpdb::Int16FromDatum(datum));
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateInt4DatumToDXL
//
//	@doc:
//		Translate a datum of type int4
//---------------------------------------------------------------------------
CDXLDatum *
CTranslatorScalarToDXL::TranslateInt4DatumToDXL
	(
	CMemoryPool *mp,
	const IMDType *md_type,
	BOOL is_null,
	ULONG , //len,
	Datum datum
	)
{
	GPOS_ASSERT(md_type->IsPassedByValue());
	CMDIdGPDB *mdid_old = CMDIdGPDB::CastMdid(md_type->MDId());
	CMDIdGPDB *mdid = GPOS_NEW(mp) CMDIdGPDB(*mdid_old);

	return GPOS_NEW(mp) CDXLDatumInt4(mp, mdid, is_null, gpdb::Int32FromDatum(datum));
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateInt8DatumToDXL
//
//	@doc:
//		Translate a datum of type int8
//---------------------------------------------------------------------------
CDXLDatum *
CTranslatorScalarToDXL::TranslateInt8DatumToDXL
	(
	CMemoryPool *mp,
	const IMDType *md_type,
	BOOL is_null,
	ULONG , //len,
	Datum datum
	)
{
	GPOS_ASSERT(md_type->IsPassedByValue());
	CMDIdGPDB *mdid_old = CMDIdGPDB::CastMdid(md_type->MDId());
	CMDIdGPDB *mdid = GPOS_NEW(mp) CMDIdGPDB(*mdid_old);

	return GPOS_NEW(mp) CDXLDatumInt8(mp, mdid, is_null, gpdb::Int64FromDatum(datum));
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::ExtractDoubleValueFromDatum
//
//	@doc:
//		Extract the double value of the datum
//---------------------------------------------------------------------------
CDouble
CTranslatorScalarToDXL::ExtractDoubleValueFromDatum
	(
	IMDId *mdid,
	BOOL is_null,
	BYTE *bytes,
	Datum datum
	)
{
	GPOS_ASSERT(CMDTypeGenericGPDB::HasByte2DoubleMapping(mdid));

	double d = 0;

	if (is_null)
	{
		return CDouble(d);
	}

	if (mdid->Equals(&CMDIdGPDB::m_mdid_numeric))
	{
		Numeric num = (Numeric) (bytes);
		if (NUMERIC_IS_NAN(num))
		{
			// in GPDB NaN is considered the largest numeric number.
			return CDouble(GPOS_FP_ABS_MAX);
		}

		d = gpdb::NumericToDoubleNoOverflow(num);
	}
	else if (mdid->Equals(&CMDIdGPDB::m_mdid_float4))
	{
		float4 f = gpdb::Float4FromDatum(datum);

		if (isnan(f))
		{
			d = GPOS_FP_ABS_MAX;
		}
		else
		{
			d = (double) f;
		}
	}
	else if (mdid->Equals(&CMDIdGPDB::m_mdid_float8))
	{
		d = gpdb::Float8FromDatum(datum);

		if (isnan(d))
		{
			d = GPOS_FP_ABS_MAX;
		}
	}
	else if (CMDTypeGenericGPDB::IsTimeRelatedType(mdid))
	{
		d = gpdb::ConvertTimeValueToScalar(datum, CMDIdGPDB::CastMdid(mdid)->Oid());
	}
	else if (CMDTypeGenericGPDB::IsNetworkRelatedType(mdid))
	{
		d = gpdb::ConvertNetworkToScalar(datum, CMDIdGPDB::CastMdid(mdid)->Oid());
	}

	return CDouble(d);
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::ExtractByteArrayFromDatum
//
//	@doc:
//		Extract the byte array value of the datum. The result is NULL if datum is NULL
//---------------------------------------------------------------------------
BYTE *
CTranslatorScalarToDXL::ExtractByteArrayFromDatum
	(
	CMemoryPool *mp,
	const IMDType *md_type,
	BOOL is_null,
	ULONG len,
	Datum datum
	)
{
	ULONG length = 0;
	BYTE *bytes = NULL;

	if (is_null)
	{
		return bytes;
	}

	length = (ULONG) gpdb::DatumSize(datum, md_type->IsPassedByValue(), len);
	GPOS_ASSERT(length > 0);

	bytes = GPOS_NEW_ARRAY(mp, BYTE, length);

	if (md_type->IsPassedByValue())
	{
		GPOS_ASSERT(length <= ULONG(sizeof(Datum)));
		clib::Memcpy(bytes, &datum, length);
	}
	else
	{
		clib::Memcpy(bytes, gpdb::PointerFromDatum(datum), length);
	}

	return bytes;
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::ExtractLintValueFromDatum
//
//	@doc:
//		Extract the long int value of a datum
//---------------------------------------------------------------------------
LINT
CTranslatorScalarToDXL::ExtractLintValueFromDatum
	(
	IMDId *mdid,
	BOOL is_null,
	BYTE *bytes,
	ULONG length
	)
{
	GPOS_ASSERT(CMDTypeGenericGPDB::HasByte2IntMapping(mdid));

	LINT lint_value = 0;
	if (is_null)
	{
		return lint_value;
	}

	if (mdid->Equals(&CMDIdGPDB::m_mdid_cash))
	{
		// cash is a pass-by-ref type
		Datum datumConstVal = (Datum) 0;
		clib::Memcpy(&datumConstVal, bytes, length);
		// Date is internally represented as an int32
		lint_value = (LINT) (gpdb::Int32FromDatum(datumConstVal));

	}
	else
	{
		// use hash value
		ULONG hash = 0;
		if (is_null)
		{
			hash = gpos::HashValue<ULONG>(&hash);
		}
		else
		{
			if (mdid->Equals(&CMDIdGPDB::m_mdid_bpchar))
			{
				hash = gpdb::HashBpChar((Datum) bytes);
			}
			else
			{
				hash = gpdb::HashText((Datum) bytes);
			}
		}

		lint_value = (LINT) hash;
	}

	return lint_value;
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::CreateIDatumFromGpdbDatum
//
//	@doc:
//		Create IDatum from GPDB datum
//---------------------------------------------------------------------------
IDatum *
CTranslatorScalarToDXL::CreateIDatumFromGpdbDatum
	(
	CMemoryPool *mp,
	const IMDType *md_type,
	BOOL is_null,
	Datum gpdb_datum
	)
{
	ULONG length = md_type->Length();
	if (!md_type->IsPassedByValue() && !is_null)
	{
		INT len = dynamic_cast<const CMDTypeGenericGPDB *>(md_type)->GetGPDBLength();
		length = (ULONG) gpdb::DatumSize(gpdb_datum, md_type->IsPassedByValue(), len);
	}
	GPOS_ASSERT(is_null || length > 0);

	CDXLDatum *datum_dxl = CTranslatorScalarToDXL::TranslateDatumToDXL(mp, md_type, gpmd::default_type_modifier, is_null, length, gpdb_datum);
	IDatum *datum = md_type->GetDatumForDXLDatum(mp, datum_dxl);
	datum_dxl->Release();
	return datum;
}

// EOF
