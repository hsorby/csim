
#include "csim_config.h"
#include "cellml_model_definition.h"

#include <iostream>
#include <sstream>
#include <string>
#include <locale>
#ifdef CSIM_HAVE_STD_CODECVT
#  include <codecvt>
#else
#  include <wchar.h>
#endif

#include <IfaceCellML_APISPEC.hxx>
#include <IfaceCCGS.hxx>
#include <CeVASBootstrap.hpp>
#include <MaLaESBootstrap.hpp>
#include <CCGSBootstrap.hpp>
#include <CellMLBootstrap.hpp>
#include <IfaceAnnoTools.hxx>
#include <AnnoToolsBootstrap.hpp>
#include <cellml-api-cxx-support.hpp>

#include "csim/error_codes.h"

/*
 * Prototype local methods
 */
static std::wstring s2ws(const std::string& str);
static std::string ws2s(const std::wstring& wstr);
static int flagVariable(const std::string& variableId, unsigned char type,
                        std::vector<iface::cellml_services::VariableEvaluationType> vets,
                        int& count, CellmlApiObjects* capi,
                        std::map<std::string, unsigned char>& variableTypes,
                        std::map<std::string, std::map<unsigned char, int> >& variableIndices);
static ObjRef<iface::cellml_api::CellMLVariable> findLocalVariable(CellmlApiObjects* capi, const std::string& variableId);
static std::string generateCodeForModel(CellmlApiObjects* capi,
                                        std::map<std::string, unsigned char>& variableTypes,
                                        std::map<std::string, std::map<unsigned char, int> >& variableIndices,
                                        int numberOfInputs, int numberOfStates);
typedef std::pair<std::string, std::string> CVpair;
static CVpair splitName(const std::string& s);
static std::string clearCodeAssignments(const std::string& s, const std::string& array, int count);

// need a method to uniquely identify variables by string, using the objid directly seemed
// to give random overlaps. But separating out like this seems to have resolved the issue?
static std::string getVariableUniqueId(iface::cellml_api::CellMLVariable* variable)
{
    std::string id = variable->objid();
    return id;
}

/**
 * In order to be flexible in allowing a single variable to be multiple types (state
 * and output; input and output; etc.) we use these bit flags.
 * http://www.cplusplus.com/forum/general/1590/
 */
enum VariableTypes {
    UndefinedType    = 0x01,
    StateType        = 0x02,
    InputType        = 0x04,
    OutputType       = 0x08,
    IndependentType  = 0x10
  //OpDaylight      = 0x20
};

class CellmlApiObjects
{
public:
    ObjRef<iface::cellml_api::Model> model;
    ObjRef<iface::cellml_services::AnnotationSet> annotations;
    ObjRef<iface::cellml_services::CeVAS> cevas;
    ObjRef<iface::cellml_services::CodeInformation> codeInformation;
};

CellmlModelDefinition::CellmlModelDefinition() : mUrl(""), mModelLoaded(false), mCapi(0)
{
    mNumberOfIndependentVariables = 0;
    mNumberOfInputVariables = 0;
    mNumberOfOutputVariables = 0;
    mStateCounter = 0;
}

CellmlModelDefinition::~CellmlModelDefinition()
{
    if (mCapi)
    {
        delete mCapi;
        mCapi = NULL;
    }
}

int CellmlModelDefinition::loadModel(const std::string &url)
{
    std::cout << "Creating CellML Model Definition from the URL: "
              << url << std::endl;
    mUrl = url;
    if (mUrl.empty()) return -1;
    std::wstring urlW = s2ws(url);
    ObjRef<iface::cellml_api::CellMLBootstrap> cb = CreateCellMLBootstrap();
    ObjRef<iface::cellml_api::DOMModelLoader> ml = cb->modelLoader();
    try
    {
        ObjRef<iface::cellml_api::Model> model = ml->loadFromURL(urlW);
        model->fullyInstantiateImports();
        // we have a model, so we can start grabbing hold of the CellML API objects
        mCapi = new CellmlApiObjects();
        mCapi->model = model;
        // create an annotation set to manage our variable usages
        ObjRef<iface::cellml_services::AnnotationToolService> ats = CreateAnnotationToolService();
        ObjRef<iface::cellml_services::AnnotationSet> as = ats->createAnnotationSet();
        mCapi->annotations = as;
        // mapping the connections between variables is a very expensive operation, so we want to
        // only do it once and keep hold of the mapping (tracker item 3294)
        ObjRef<iface::cellml_services::CeVASBootstrap> cvbs = CreateCeVASBootstrap();
        ObjRef<iface::cellml_services::CeVAS> cevas = cvbs->createCeVASForModel(model);
        std::wstring msg = cevas->modelError();
        if (msg != L"")
        {
            std::cerr << "loadModel: Error creating CellML Variable Association Service: "
                      << ws2s(msg) << std::endl;
            return -2;
        }
        mCapi->cevas = cevas;
        // now check we can generate code and grab hold of the initial code information
        ObjRef<iface::cellml_services::CodeGeneratorBootstrap> cgb = CreateCodeGeneratorBootstrap();
        ObjRef<iface::cellml_services::CodeGenerator> cg = cgb->createCodeGenerator();
        try
        {
            cg->useCeVAS(cevas);
            ObjRef<iface::cellml_services::CodeInformation> cci = cg->generateCode(model);
            msg = cci->errorMessage();
            if (msg != L"")
            {
                std::cerr << "CellmlModelDefintion::loadModel: Error generating code: "
                          << ws2s(msg) << std::endl;
                return -4;
            }
            // TODO: we are only interested in models we can work with?
            if (cci->constraintLevel() != iface::cellml_services::CORRECTLY_CONSTRAINED)
            {
                std::cerr << "CellmlModelDefintion::loadModel: Model is not correctly constrained: "
                          << std::endl;
                return -5;
            }
            mCapi->codeInformation = cci;
            // flag all state variables and the variable of integration
            ObjRef<iface::cellml_services::ComputationTargetIterator> cti = cci->iterateTargets();
            while (true)
            {
                ObjRef<iface::cellml_services::ComputationTarget> ct = cti->nextComputationTarget();
                if (ct == NULL) break;
                if (ct->degree() > 0) break; // only want to initialise the base variables not the differential
                ObjRef<iface::cellml_api::CellMLVariable> v(ct->variable());
                if (ct->type() == iface::cellml_services::STATE_VARIABLE)
                {
                    mVariableTypes[getVariableUniqueId(v)] = StateType;
                    mVariableIndices[getVariableUniqueId(v)][StateType] = mStateCounter;
                    mStateCounter++;
                }
                else if (ct->type() == iface::cellml_services::VARIABLE_OF_INTEGRATION)
                {
                    mVariableTypes[getVariableUniqueId(v)] = IndependentType;
                    mNumberOfIndependentVariables++;
                }
                else
                {
                    // need to initialise the variable type
                    mVariableTypes[getVariableUniqueId(v)] = UndefinedType;
                }
            }
        }
        catch (...)
        {
            std::cerr << "loadModel: Error generating the code information for the model" << std::endl;
            return -3;
        }
        // if we get to here, everything worked.
        mModelLoaded = true;
    }
    catch (...)
    {
      std::wcerr << L"Error loading model: " << urlW << std::endl;
      return -1;
    }
    return csim::CSIM_OK;
}

int CellmlModelDefinition::setVariableAsInput(const std::string &variableId)
{
    std::cout << "CellmlModelDefinition::setVariableAsInput: flagging variable: " << variableId
              << "; as a INPUT variable." << std::endl;
    std::vector<iface::cellml_services::VariableEvaluationType> vets;
    // initially, only allow "constant" variables to be defined externally
    vets.push_back(iface::cellml_services::CONSTANT);
    //vets.push_back(iface::cellml_services::VARIABLE_OF_INTEGRATION);
    //vets.push_back(iface::cellml_services::FLOATING);
    int index = flagVariable(variableId, InputType, vets,
                             mNumberOfInputVariables, mCapi,
                             mVariableTypes, mVariableIndices);
    return index;
}

int CellmlModelDefinition::setVariableAsOutput(const std::string &variableId)
{
    std::cout << "CellmlModelDefinition::setVariableAsOutput: flagging variable: " << variableId
              << "; as a OUTPUT variable." << std::endl;
    std::vector<iface::cellml_services::VariableEvaluationType> vets;
    // state variables should be flagged if they need to be copied into the wanted array
    vets.push_back(iface::cellml_services::STATE_VARIABLE);
    vets.push_back(iface::cellml_services::PSEUDOSTATE_VARIABLE);
    vets.push_back(iface::cellml_services::ALGEBRAIC);
    //vets.push_back(iface::cellml_services::LOCALLY_BOUND);
    // we need to allow constant variables to be flagged as wanted since if it is a model with no
    // differential equations then all algebraic variables will be constant - i.e., constitutive laws
    vets.push_back(iface::cellml_services::CONSTANT);
    vets.push_back(iface::cellml_services::VARIABLE_OF_INTEGRATION);
    int index = flagVariable(variableId, OutputType, vets,
                             mNumberOfOutputVariables, mCapi, mVariableTypes,
                             mVariableIndices);
    return index;
}

int CellmlModelDefinition::instantiate(Compiler& compiler)
{
    std::string codeString = generateCodeForModel(mCapi, mVariableTypes, mVariableIndices,
                                                  mNumberOfInputVariables,
                                                  mStateCounter);
    if (compiler.isVerbose())
    {
        std::cout << "Code string:\n***********************\n" << codeString << "\n#####################################\n"
                  << std::endl;
    }
    return compiler.compileCodeString(codeString);
}

std::wstring s2ws(const std::string& str)
{
#ifdef CSIM_HAVE_STD_CODECVT
    typedef std::codecvt_utf8<wchar_t> convert_typeX;
    std::wstring_convert<convert_typeX, wchar_t> converterX;
    return converterX.from_bytes(str);
#else
    std::wstring ss;
    if (str.length() > 0)
    {
        const char* sp = str.c_str();
        wchar_t* s;
        size_t l = strlen(sp);
        s = (wchar_t*)malloc(sizeof(wchar_t)*(l+1));
        memset(s,0,(l+1)*sizeof(wchar_t));
        mbsrtowcs(s,&sp,l,NULL);
        ss = std::wstring(s);
        free(s);
        return(ss);
    }
    return(ss);
#endif
}

std::string ws2s(const std::wstring& wstr)
{
#ifdef CSIM_HAVE_STD_CODECVT
    typedef std::codecvt_utf8<wchar_t> convert_typeX;
    std::wstring_convert<convert_typeX, wchar_t> converterX;
    return converterX.to_bytes(wstr);
#else
    std::string ss;
    if (wstr.length() > 0)
    {
        const wchar_t* sp = wstr.c_str();
        size_t len = wcsrtombs(NULL,&sp,0,NULL);
        if (len > 0)
        {
            len++;
            char* s = (char*)malloc(len);
            wcsrtombs(s,&sp,len,NULL);
            ss = std::string(s);
            free(s);
            return(ss);
        }
    }
    return(ss);
#endif
}

int flagVariable(const std::string& variableId, unsigned char type,
                 std::vector<iface::cellml_services::VariableEvaluationType> vets,
                 int& count, CellmlApiObjects* capi,
                 std::map<std::string, unsigned char>& variableTypes,
                 std::map<std::string, std::map<unsigned char, int> >& variableIndices)
{
    if (! capi->codeInformation)
    {
        std::cerr << "CellML Model Definition::flagVariable: missing model implementation?" << std::endl;
        return csim::UNABLE_TO_FLAG_VARIABLE;
    }
    ObjRef<iface::cellml_api::CellMLVariable> sv = findLocalVariable(capi, variableId);
    if (!sv)
    {
        std::cerr << "CellML Model Definition::flagVariable: unable to find source variable for: "
                  << variableId << std::endl;
        return csim::UNABLE_TO_FLAG_VARIABLE;
    }

    // FIXME: here we simply accept any flag combination. Code generation is the place
    // where flags will be checked for consistency?

    // check if source is already flagged with the specified type.
    std::map<std::string, unsigned char>::iterator currentAnnotation =
            variableTypes.find(getVariableUniqueId(sv));
    unsigned char currentTypes;
    if (currentAnnotation != variableTypes.end())
    {
        currentTypes = currentAnnotation->second;
        if (currentTypes & type)
        {
            std::cout << "Already flagged same type, nothing to do." << std::endl;
            return variableIndices[getVariableUniqueId(sv)][type];
        }
    }
    // find corresponding computation target
    ObjRef<iface::cellml_services::ComputationTargetIterator> cti =
            capi->codeInformation->iterateTargets();
    ObjRef<iface::cellml_services::ComputationTarget> ct(NULL);
    while(true)
    {
        ct = cti->nextComputationTarget();
        if (ct == NULL) break;
        if (ct->variable() == sv) break;
    }
    if (!ct)
    {
        std::cerr << "CellMLModelDefinition::flagVariable -- unable get computation target for the source of variable: "
                  << variableId << std::endl;
        return csim::NO_MATCHING_COMPUTATION_TARGET;
    }

    // check type of computation target and make sure compatible
    unsigned int i,compatible = 0;
    for (i=0; i<vets.size(); i++)
    {
        if (ct->type() == vets[i])
        {
            compatible = 1;
            break;
        }
    }
    if (!compatible)
    {
        std::cerr << "CellMLModelDefinition::flagVariable -- computation target for variable: "
                  << variableId << "; is the wrong type to be flagged" << std::endl;
        std::cerr << "Computation target for this source variable is: ";
        switch (ct->type())
        {
        case iface::cellml_services::CONSTANT:
            std::cerr << "CONSTANT";
            break;
        case iface::cellml_services::VARIABLE_OF_INTEGRATION:
            std::cerr << "VARIABLE_OF_INTEGRATION";
            break;
        case iface::cellml_services::STATE_VARIABLE:
            std::cerr << "STATE_VARIABLE";
            break;
        case iface::cellml_services::PSEUDOSTATE_VARIABLE:
            std::cerr << "PSEUDOSTATE_VARIABLE";
            break;
        case iface::cellml_services::ALGEBRAIC:
            std::cerr << "ALGEBRAIC";
            break;
        case iface::cellml_services::LOCALLY_BOUND:
            std::cerr << "LOCALLY_BOUND";
            break;
        case iface::cellml_services::FLOATING:
            std::cerr << "FLOATING";
            break;
        default:
            std::cerr << "Invalid";
        }
        std::cerr << std::endl;
        return csim::MISMATCHED_COMPUTATION_TARGET;
    }
    variableTypes[getVariableUniqueId(sv)] = currentTypes | type;
    variableIndices[getVariableUniqueId(sv)][type] = count++;
    return variableIndices[getVariableUniqueId(sv)][type];
}

ObjRef<iface::cellml_api::CellMLVariable> findLocalVariable(CellmlApiObjects* capi, const std::string& variableId)
{
    if (!(capi->cevas))
    {
        std::cerr << "CellMLModelDefinition::findLocalVariable -- missing CeVAS object?" << std::endl;
        return NULL;
    }
    // find named variable - in local components only!
    CVpair cv = splitName(variableId);
    if ((cv.first.length() == 0) || (cv.second.length() == 0)) return NULL;
    //std::cout << "Component name: " << cv.first << "; variable name: " << cv.second << std::endl;
    ObjRef<iface::cellml_api::CellMLComponentSet> components = capi->model->localComponents();
    ObjRef<iface::cellml_api::CellMLComponent> component = components->getComponent(s2ws(cv.first));
    if (!component)
    {
        std::cerr << "CellMLModelDefinition::findLocalVariable -- unable to find local component: " << cv.first << std::endl;
        return NULL;
    }
    ObjRef<iface::cellml_api::CellMLVariableSet> variables = component->variables();
    ObjRef<iface::cellml_api::CellMLVariable> variable = variables->getVariable(s2ws(cv.second));
    if (!variable)
    {
        std::cerr << "CellMLModelDefinition::findLocalVariable -- unable to find variable: " << cv.first << " / "
                  << cv.second << std::endl;
        return NULL;
    }
    // get source variable
    ObjRef<iface::cellml_services::ConnectedVariableSet> cvs = capi->cevas->findVariableSet(variable);
    ObjRef<iface::cellml_api::CellMLVariable> v = cvs->sourceVariable();
    if (!v)
    {
        std::cerr << "CellMLModelDefinition::findLocalVariable -- unable get source variable for variable: "
                  << cv.first << " / " << cv.second << std::endl;
        return NULL;
    }
    return v;
}

CVpair splitName(const std::string& s)
{
    CVpair p;
    std::size_t pos = s.find('/');
    if (pos == std::string::npos) return p;
    p.first = s.substr(0, pos);
    p.second = s.substr(pos+1);
    return p;
}

std::string generateCodeForModel(CellmlApiObjects* capi,
                                 std::map<std::string, unsigned char>& variableTypes,
                                 std::map<std::string, std::map<unsigned char, int> >& variableIndices,
                                 int numberOfInputs, int numberOfStates)
{
    std::stringstream code;
    std::string codeString;
    if (! (capi && capi->codeInformation))
    {
        std::cerr << "CellML Model Definition::generateCodeForModel: missing model implementation?" << std::endl;
        return "";
    }
    // We need to regenerate the code information to make use of the flagged variables.
    ObjRef<iface::cellml_services::CodeGeneratorBootstrap> cgb = CreateCodeGeneratorBootstrap();
    ObjRef<iface::cellml_services::CodeGenerator> cg = cgb->createCodeGenerator();
    // catch any exceptions from the CellML API
    try
    {
        // annotate the source variables in the model with the code-names based on existing annotations
        for (unsigned int i=0; i < capi->cevas->length(); i++)
        {
            ObjRef<iface::cellml_services::ConnectedVariableSet> cvs = capi->cevas->getVariableSet(i);
            ObjRef<iface::cellml_api::CellMLVariable> sv = cvs->sourceVariable();
            std::string currentId = getVariableUniqueId(sv);
            std::map<std::string, unsigned char>::iterator typeit(variableTypes.find(currentId));
            if (typeit != variableTypes.end())
            {
                std::wstringstream ename;
                // here we assign an array and index based on the "primary" purpose of the variable. Later
                // we will add in secondary purposes.
                unsigned char vType = typeit->second;
                if (vType & StateType)
                {
                    ename << L"CSIM_STATE[" << variableIndices[currentId][StateType] << L"]";
                }
                else if (vType & IndependentType)
                {
                    // do nothing, but stop input and output annotations
                }
                else if (vType & InputType)
                {
                    ename << L"CSIM_INPUT[" << variableIndices[currentId][InputType] << L"]";
                }
                else if (vType & OutputType)
                {
                    ename << L"CSIM_OUTPUT[" << variableIndices[currentId][OutputType] << L"]";
                }
                capi->annotations->setStringAnnotation(sv, L"expression", ename.str());

                if (vType & StateType)
                {
                    ename.str(L"");
                    ename.clear();
                    ename << L"CSIM_RATE[" << variableIndices[currentId][StateType] << L"]";
                    capi->annotations->setStringAnnotation(sv, L"expression_d1", ename.str());
                }
            }
        }
        cg->useCeVAS(capi->cevas);
        cg->useAnnoSet(capi->annotations);
        ObjRef<iface::cellml_services::CodeInformation> cci = cg->generateCode(capi->model);
        std::wstring m = cci->errorMessage();
        if (m != L"")
        {
            std::cerr << "CellML Model Definition::generateCodeForModel: error generating code?" << std::endl;
            return "";
        }
        iface::cellml_services::ModelConstraintLevel mcl = cci->constraintLevel();
        if (mcl == iface::cellml_services::UNDERCONSTRAINED)
        {
            std::cerr << "Model is underconstrained" << std::endl;
            return "";
        }
        else if (mcl == iface::cellml_services::OVERCONSTRAINED)
        {
            std::cerr << "Model is overconstrained" << std::endl;
            return "";
        }
        else if (mcl == iface::cellml_services::UNSUITABLY_CONSTRAINED)
        {
            std::cerr << "Model is unsuitably constrained" << std::endl;
            return "";
        }
        std::cout << "Model is correctly constrained" << std::endl;
        // create the code in the format we know how to handle
        code << "#include <math.h>\n"
        /* required functions */
             << "extern double fabs(double x);\n"
             << "extern double acos(double x);\n"
             << "extern double acosh(double x);\n"
             << "extern double atan(double x);\n"
             << "extern double atanh(double x);\n"
             << "extern double asin(double x);\n"
             << "extern double asinh(double x);\n"
             << "extern double acos(double x);\n"
             << "extern double acosh(double x);\n"
             << "extern double asin(double x);\n"
             << "extern double asinh(double x);\n"
             << "extern double atan(double x);\n"
             << "extern double atanh(double x);\n"
             << "extern double ceil(double x);\n"
             << "extern double cos(double x);\n"
             << "extern double cosh(double x);\n"
             << "extern double tan(double x);\n"
             << "extern double tanh(double x);\n"
             << "extern double sin(double x);\n"
             << "extern double sinh(double x);\n"
             << "extern double exp(double x);\n"
             << "extern double floor(double x);\n"
             << "extern double pow(double x, double y);\n"
             << "extern double factorial(double x);\n"
             << "extern double log(double x);\n"
             << "extern double arbitrary_log(double x, double base);\n"
             << "extern double gcd_pair(double a, double b);\n"
             << "extern double lcm_pair(double a, double b);\n"
             << "extern double gcd_multi(unsigned int size, ...);\n"
             << "extern double lcm_multi(unsigned int size, ...);\n"
             << "extern double multi_min(unsigned int size, ...);\n"
             << "extern double multi_max(unsigned int size, ...);\n"
             << "extern void NR_MINIMISE(double(*func)"
                "(double VOI, double *C, double *R, double *S, double *A),"
                "double VOI, double *C, double *R, double *S, double *A, "
                "double *V);\n";
        std::wstring frag = cci->functionsString();
        code << ws2s(frag);

        int nAlgebraic = cci->algebraicIndexCount();
        int nConstants = cci->constantIndexCount();

        code << "\n\nvoid csim_rhs_routine(double VOI, double* CSIM_STATE, double* CSIM_RATE, double* CSIM_OUTPUT, "
             << "double* CSIM_INPUT)\n{\n\n"
             << "double DUMMY_ASSIGNMENT;\n"
             << "double CONSTANTS["
             << nConstants
             << "], ALGEBRAIC["
             << nAlgebraic
             << "];\n\n";

        /* initConsts - all variables which aren't state variables but have
         *              an initial_value attribute, and any variables & rates
         *              which follow.
         */
        code << ws2s(cci->initConstsString());

        /* rates      - All rates which are not static.
         */
        code << ws2s(cci->ratesString());

        /* variables  - All variables not computed by initConsts or rates
         *  (i.e., these are not required for the integration of the model and
         *   thus only need to be called for output or presentation or similar
         *   purposes)
         */
        code << ws2s(cci->variablesString());

        // add in the setting of any outputs that are not already defined
        for (unsigned int i=0; i < capi->cevas->length(); i++)
        {
            ObjRef<iface::cellml_services::ConnectedVariableSet> cvs = capi->cevas->getVariableSet(i);
            ObjRef<iface::cellml_api::CellMLVariable> sv = cvs->sourceVariable();
            std::string currentId = getVariableUniqueId(sv);
            std::map<std::string, unsigned char>::iterator typeit(variableTypes.find(currentId));
            if (typeit != variableTypes.end())
            {
                unsigned char vType = typeit->second;
                if (vType & OutputType)
                {
                    if (vType & StateType)
                        code << "CSIM_OUTPUT[" << variableIndices[currentId][OutputType]
                                << "] = CSIM_STATE[" << variableIndices[currentId][StateType]
                                   << "];\n";
                    else if (vType & InputType)
                        code << "CSIM_OUTPUT[" << variableIndices[currentId][OutputType]
                                << "] = CSIM_INPUT[" << variableIndices[currentId][InputType]
                                   << "];\n";
                    else if (vType & IndependentType)
                        code << "CSIM_OUTPUT[" << variableIndices[currentId][OutputType]
                                << "] = VOI;\n";
                }
            }
        }

        // close the subroutine
        code << "\n\n}//csim_rhs_routine()\n\n";

        // and now clear out initialisation of state variables and known variables from
        // the RHS routine.
        codeString = clearCodeAssignments(code.str(), "CSIM_STATE", numberOfStates);
        codeString = clearCodeAssignments(codeString, "CSIM_INPUT", numberOfInputs);

        // and finally create the initialisation routine
        std::stringstream initRoutine;
        initRoutine << "\nvoid csim_initialise_routine(double* CSIM_STATE, double* CSIM_OUTPUT, double* CSIM_INPUT)\n{\n";
        // FIXME: this doesn't need to be in the interface?
        initRoutine << "double CSIM_RATES[" << numberOfStates << "];\n";
        initRoutine << "double CONSTANTS[" << nConstants << "];\n";
        initRoutine << ws2s(cci->initConstsString());
        initRoutine << "\n}\n";

        codeString += initRoutine.str();
    }
    catch (...)
    {
        std::cerr << "CellML Model Definition::generateCodeForModel: something went wrong generating code?" << std::endl;
        return "";
    }
    return codeString;
}

std::string clearCodeAssignments(const std::string& s, const std::string& array, int count)
{
    std::string code(s);
    for (int i=0; i < count; i++)
    {
        std::stringstream search;
        search << array;
        search << "[" << i << "]";
        std::string r("DUMMY_ASSIGNMENT /*");
        r += search.str();
        search << " = ";
        r += "*/ = ";
        if (code.find(search.str()) != std::string::npos)
            code.replace(code.find(search.str()), search.str().size(), r);
    }
    return code;
}
