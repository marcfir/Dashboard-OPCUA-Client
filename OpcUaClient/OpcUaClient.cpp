#include "OpcUaClient.hpp"

#include <iostream>
#include "SetupSecurity.hpp"
#include <easylogging++.h>

#include <list>


#include "uaplatformlayer.h"

#include <Exceptions/ClientNotConnected.hpp>
#include "Exceptions/OpcUaNonGoodStatusCodeException.hpp"

#include "Converter/ModelNodeIdToUaNodeId.hpp"
#include "Converter/UaNodeIdToModelNodeId.hpp"
#include "Converter/ModelQualifiedNameToUaQualifiedName.hpp"
#include "Converter/UaQualifiedNameToModelQualifiedName.hpp"
#include "Converter/UaNodeClassToModelNodeClass.hpp"
#include "Converter/UaDataValueToJsonValue.hpp"


namespace Umati {

	namespace OpcUa {

		int OpcUaClient::PlattformLayerInitialized = 0;

		OpcUaClient::OpcUaClient(std::string serverURI, std::string Username, std::string Password, std::uint8_t security, std::vector<std::string> expectedObjectTypeNamespaces, std::shared_ptr<Umati::OpcUa::OpcUaInterface> opcUaWrapper)
			: m_serverUri(serverURI), m_username(Username), m_password(Password), m_expectedObjectTypeNamespaces(expectedObjectTypeNamespaces), m_security(static_cast<OpcUa_MessageSecurityMode>(security)), m_subscr(m_uriToIndexCache, m_indexToUriCache)
		{
			m_defaultServiceSettings.callTimeout = 10000;
            m_opcUaWrapper = std::move(opcUaWrapper);
            m_opcUaWrapper->setSubscription(&m_subscr);

			if (++PlattformLayerInitialized == 1)
			{
				UaPlatformLayer::init();
			}

			m_tryConnecting = true;
			// Try connecting at least once
			this->connect();
			m_connectThread = std::make_shared<std::thread>([this]() {this->threadConnectExecution(); });

		}

		bool OpcUaClient::connect()
		{
			UaString sURL(m_serverUri.c_str());
			UaStatus result;

            UaClientSdk::SessionSecurityInfo sessionSecurityInfo;
			UaClientSdk::ServiceSettings serviceSettings;
			UaEndpointDescriptions endpointDescriptions;
			UaApplicationDescriptions applicationDescriptions;
            SetupSecurity::setupSecurity(&sessionSecurityInfo);

            result = m_opcUaWrapper->DiscoveryGetEndpoints(serviceSettings, sURL, sessionSecurityInfo,endpointDescriptions);
            if (result.isBad())
			{
                return false;
			}

			struct {
				UaString url;
				UaByteString serverCertificate;
				UaString securityPolicy;
				OpcUa_UInt32 securityMode;
			} desiredEndpoint;


			auto desiredSecurity = m_security;
			for (OpcUa_UInt32 iEndpoint = 0; iEndpoint < endpointDescriptions.length(); iEndpoint++)
			{

				if (endpointDescriptions[iEndpoint].SecurityMode != desiredSecurity)
				{
					continue;
				}
                desiredEndpoint.url = UaString(endpointDescriptions[iEndpoint].EndpointUrl);
				LOG(INFO) << "desiredEndpoint.url: " << desiredEndpoint.url.toUtf8() << std::endl;
				sessionSecurityInfo.serverCertificate = endpointDescriptions[iEndpoint].ServerCertificate;
				sessionSecurityInfo.sSecurityPolicy = endpointDescriptions[iEndpoint].SecurityPolicyUri;
				sessionSecurityInfo.messageSecurityMode = static_cast<OpcUa_MessageSecurityMode>(endpointDescriptions[iEndpoint].SecurityMode);
				break;
			}

			if (desiredEndpoint.url.isEmpty())
			{
			    LOG(ERROR) << "Could not find endpoint without encryption." << std::endl;
			    return false;
			}

			///\todo handle security
			sessionSecurityInfo.doServerCertificateVerify = OpcUa_False;
			sessionSecurityInfo.disableErrorCertificateHostNameInvalid = OpcUa_True;
			sessionSecurityInfo.disableApplicationUriCheck = OpcUa_True;

			if (!m_username.empty() && !m_password.empty())
			{
				sessionSecurityInfo.setUserPasswordUserIdentity(m_username.c_str(), m_password.c_str());
			}

			m_opcUaWrapper->GetNewSession(m_pSession);

            UaClientSdk::SessionConnectInfo sessionConnectInfo;
            sessionConnectInfo = prepareSessionConnectInfo(sessionConnectInfo);

			result = m_opcUaWrapper->SessionConnect(sURL, sessionConnectInfo, sessionSecurityInfo, this);

			if (!result.isGood())
			{
				LOG(ERROR) << "Connecting failed in OPC UA Data Client: " << result.toString().toUtf8() << std::endl;
				return false;
			}
            on_connected();

            return true;
		}

        UaClientSdk::SessionConnectInfo &
        OpcUaClient::prepareSessionConnectInfo(UaClientSdk::SessionConnectInfo &sessionConnectInfo) const {
            sessionConnectInfo.sApplicationName = "KonI4.0 OPC UA Data Client";
            sessionConnectInfo.sApplicationUri = "http://dashboard.umati.app/OPCUA_DataClient";
            sessionConnectInfo.sProductUri = "KonI40OpcUaClient_Product";
            sessionConnectInfo.sSessionName = "DefaultSession";
            return sessionConnectInfo;
        }

        void OpcUaClient::on_connected() {
            updateNamespaceCache();
            m_opcUaWrapper->SubscriptionCreateSubscription(m_pSession);
        }

        OpcUa_NodeClass OpcUaClient::readNodeClass(UaNodeId nodeId)
		{
			checkConnection();

			UaReadValueIds readValueIds;
			readValueIds.create(1);
			nodeId.copyTo(&readValueIds[0].NodeId);
			readValueIds[0].AttributeId = OpcUa_Attributes_NodeClass;

			UaDataValues readResult;

			UaDiagnosticInfos diagInfo;

			auto uaResult = m_opcUaWrapper->SessionRead(
				m_defaultServiceSettings,
				100.0,
				OpcUa_TimestampsToReturn_Neither,
				readValueIds,
				readResult,
				diagInfo
			);

			if (uaResult.isBad())
			{
				LOG(ERROR) << "readNodeClass failed for node: '" << nodeId.toXmlString().toUtf8()
					<< "' with " << uaResult.toString().toUtf8();
				throw Exceptions::OpcUaNonGoodStatusCodeException(uaResult);
			}

			if (readResult.length() != 1)
			{
				LOG(ERROR) << "readResult.length() expect 1  got:" << readResult.length();
				throw Exceptions::UmatiException("Length mismatch");
			}

			UaStatusCode uaResultElement(readResult[0].StatusCode);
			if (uaResultElement.isBad())
			{
				LOG(ERROR) << "Bad value status code failed for node: '" << nodeId.toXmlString().toUtf8()
					<< "' with " << uaResultElement.toString().toUtf8();
				throw Exceptions::OpcUaNonGoodStatusCodeException(uaResultElement);
			}

			UaVariant value(readResult[0].Value);
			if (value.type() != OpcUaType_Int32)
			{
				LOG(ERROR) << "Expect Type Int32, got '" << value.type();
				throw Exceptions::UmatiException("Type mismatch");
			}

			OpcUa_Int32 nodeClass;
			value.toInt32(nodeClass);

			return  static_cast<OpcUa_NodeClass>(nodeClass);
		}

		void OpcUaClient::checkConnection()
		{
			if (!this->m_isConnected || !m_opcUaWrapper->SessionIsConnected())
			{
				throw Exceptions::ClientNotConnected("Need connected client.");
			}
		}

		UaNodeId OpcUaClient::browseSuperType(UaNodeId typeNodeId)
		{
			checkConnection();

			auto referenceTypeUaNodeId = UaNodeId(OpcUaId_HasSubtype);


			UaClientSdk::BrowseContext browseContext;
			browseContext.browseDirection = OpcUa_BrowseDirection_Inverse;
			browseContext.includeSubtype = OpcUa_True;
			browseContext.maxReferencesToReturn = 0;
			browseContext.nodeClassMask = 0; // ALL
			browseContext.referenceTypeId = referenceTypeUaNodeId;
			browseContext.resultMask = OpcUa_BrowseResultMask_None;

			OpcUa_NodeClass nodeClass = readNodeClass(typeNodeId);

			switch (nodeClass)
			{
			case OpcUa_NodeClass_ObjectType:
			case OpcUa_NodeClass_VariableType:
			{
				browseContext.nodeClassMask = nodeClass;
				break;
			}
			default:
				LOG(ERROR) << "Invalid NodeClass " << nodeClass;
				throw Exceptions::UmatiException("Invalid NodeClass");
			}

			UaByteString continuationPoint;
			UaReferenceDescriptions referenceDescriptions;
			auto uaResult = m_opcUaWrapper->SessionBrowse(m_defaultServiceSettings, typeNodeId, browseContext, continuationPoint, referenceDescriptions);

			if (uaResult.isBad())
			{
				LOG(ERROR) << "Bad return from browse: " << uaResult.toString().toUtf8();
				throw Exceptions::OpcUaNonGoodStatusCodeException(uaResult);
			}

			std::list < ModelOpcUa::BrowseResult_t > browseResult;


			if (referenceDescriptions.length() == 0)
			{
				return UaNodeId();
			}

			if (referenceDescriptions.length() > 1)
			{
				LOG(ERROR) << "Found multiple superTypes for " << typeNodeId.toXmlString().toUtf8();
				return UaNodeId();
			}

			return UaNodeId(UaExpandedNodeId(referenceDescriptions[0].NodeId).nodeId());
		}

		bool OpcUaClient::isSameOrSubtype(UaNodeId expectedType, UaNodeId checkType, std::size_t maxDepth)
		{
			if (checkType.isNull())
			{
				return false;
			}

			if (expectedType == checkType)
			{
				return true;
			}

			auto it = m_superTypes.find(checkType);

			if (it != m_superTypes.end())
			{
				return isSameOrSubtype(expectedType, it->second, --maxDepth);
			}

			auto superType = browseSuperType(checkType);
			m_superTypes[checkType] = superType;
			return isSameOrSubtype(expectedType, superType, --maxDepth);
		}

		void OpcUaClient::updateNamespaceCache()
		{
            std::vector<std::string> notFoundObjectTypeNamespaces;
            UaStringArray uaNamespaces = m_opcUaWrapper->SessionGetNamespaceTable();

            initializeUpdateNamespaceCache(notFoundObjectTypeNamespaces);

            for (std::size_t i = 0; i < uaNamespaces.length(); ++i)
			{
			    auto uaNamespace = uaNamespaces[i];
			    auto uaNamespaceAsUaString = UaString(uaNamespace);
			    auto uaNamespaceUtf8 = uaNamespaceAsUaString.toUtf8();
				std::string namespaceURI(uaNamespaceUtf8);
				m_uriToIndexCache[namespaceURI] = static_cast<uint16_t>(i);
				m_indexToUriCache[static_cast<uint16_t>(i)] = namespaceURI;

                findObjectTypeNamespaces(notFoundObjectTypeNamespaces, i, namespaceURI);

                LOG(INFO) << "index: " << std::to_string(i) << ", namespaceURI: " << namespaceURI;
			}

            for(std::size_t i = 0; i < notFoundObjectTypeNamespaces.size(); ++i){
                LOG(WARNING) << "Unable to find namespace " << notFoundObjectTypeNamespaces[i];
            }
		}

        void OpcUaClient::findObjectTypeNamespaces(std::vector<std::string> &notFoundObjectTypeNamespaces, size_t i,
                                                   const std::string &namespaceURI) {
            auto it = find (notFoundObjectTypeNamespaces.begin(), notFoundObjectTypeNamespaces.end(), namespaceURI);
            if (it != notFoundObjectTypeNamespaces.end()) {
                m_availableObjectTypeNamespaces[static_cast<uint16_t>(i)] = namespaceURI;
                notFoundObjectTypeNamespaces.erase(it);
                LOG(INFO) << "Expected object type namespace " << namespaceURI << " found at index " << std::to_string(i);

                std::shared_ptr<std::map <std::string, std::shared_ptr<ModelOpcUa::StructureBiNode>>> bidirectionalTypeMap = std::make_shared<std::map <std::string, std::shared_ptr<ModelOpcUa::StructureBiNode>>>();
                UaClientSdk::BrowseContext browseContext = prepareObjectTypeContext();
                auto basicObjectTypeNode = ModelOpcUa::NodeId_t{"http://opcfoundation.org/UA/", "i=58" };
                UaNodeId startUaNodeId = Converter::ModelNodeIdToUaNodeId(basicObjectTypeNode, m_uriToIndexCache).getNodeId();
                browseTypes(bidirectionalTypeMap, browseContext, startUaNodeId, nullptr);
                std::shared_ptr<std::map <std::string, ModelOpcUa::StructureNode>> typeMap = std::make_shared<std::map <std::string,ModelOpcUa::StructureNode>>();
                createTypeMap(bidirectionalTypeMap, typeMap, 3);
                LOG(INFO) << "Delete me";
            }
        }

        void OpcUaClient::browseTypes(std::shared_ptr<std::map<std::string, std::shared_ptr<ModelOpcUa::StructureBiNode>>> bidirectionalTypeMap, UaClientSdk::BrowseContext browseContext, UaNodeId startUaNodeId, const std::shared_ptr<ModelOpcUa::StructureBiNode>& parent)  {

            // References -> nodes referenced to this, e.g. child nodes
            // BrowseName: Readable Name and namespace index
            UaByteString continuationPoint;
            UaReferenceDescriptions referenceDescriptions;
            auto uaResult = m_opcUaWrapper->SessionBrowse(m_defaultServiceSettings, startUaNodeId, browseContext, continuationPoint, referenceDescriptions);

            if (uaResult.isBad())
            {
                LOG(ERROR) << "Bad return from browse: " << uaResult.toString().toUtf8();
                throw Exceptions::OpcUaNonGoodStatusCodeException(uaResult);
            }

            for (OpcUa_UInt32 i = 0; i < referenceDescriptions.length(); i++)
            {
                ModelOpcUa::BrowseResult_t entry = ReferenceDescriptionToBrowseResult(referenceDescriptions[i]);
                auto current = handleBrowseTypeResult(bidirectionalTypeMap, startUaNodeId, referenceDescriptions, i, entry, parent);
                UaNodeId nextUaNodeId = Converter::ModelNodeIdToUaNodeId(entry.NodeId, m_uriToIndexCache).getNodeId();
                browseTypes(bidirectionalTypeMap, browseContext, nextUaNodeId, current);
            }
        }

        std::shared_ptr<ModelOpcUa::StructureBiNode> OpcUaClient::handleBrowseTypeResult(std::shared_ptr<std::map<std::string, std::shared_ptr<ModelOpcUa::StructureBiNode>>> &bidirectionalTypeMap,
                                            const UaNodeId &startUaNodeId,
                                            const UaReferenceDescriptions &referenceDescriptions,
                                            OpcUa_UInt32 i,
                                            const ModelOpcUa::BrowseResult_t &entry,
                                            const std::shared_ptr<ModelOpcUa::StructureBiNode>& parent) {
            UaNodeId currentUaNodeId = Converter::ModelNodeIdToUaNodeId(entry.NodeId, m_uriToIndexCache).getNodeId();
            uint16_t currentNamespaceIndex = currentUaNodeId.namespaceIndex();
            auto it = m_availableObjectTypeNamespaces.find(currentNamespaceIndex);
            bool isObjectType = ModelOpcUa::ObjectType == entry.NodeClass;
            ModelOpcUa::StructureBiNode node(entry, std::list<std::shared_ptr<const ModelOpcUa::StructureNode>>(), parent, (uint16_t) currentUaNodeId.namespaceIndex());
            auto current = std::make_shared<ModelOpcUa::StructureBiNode>(node);

            if (isObjectType) {
                std::string typeName = node.structureNode->SpecifiedBrowseName.Name;
                if(bidirectionalTypeMap->count(typeName) == 0) {
                    std::pair <std::string, std::shared_ptr<ModelOpcUa::StructureBiNode>> newType(typeName, current);
                    bidirectionalTypeMap->insert(newType);
                } else {
                    LOG(INFO) << "Found Type " << typeName << " again.";
                }
            }
            if (parent != nullptr) {
                parent->SpecifiedBiChildNodes.emplace_back(current);
            }
            return current;
       }

        UaClientSdk::BrowseContext OpcUaClient::prepareObjectTypeContext() const {
            UaClientSdk::BrowseContext browseContext;
            browseContext.browseDirection = OpcUa_BrowseDirection_Forward;
            browseContext.includeSubtype = OpcUa_True;
            browseContext.maxReferencesToReturn = 0;
            /*
             * - OpcUa_NodeClass_Object        = 1,
             * - OpcUa_NodeClass_Variable      = 2,
             * - OpcUa_NodeClass_Method        = 4,
             * - OpcUa_NodeClass_ObjectType    = 8,
             * - OpcUa_NodeClass_VariableType  = 16,
             * - OpcUa_NodeClass_ReferenceType = 32,
             * - OpcUa_NodeClass_DataType      = 64,
             * - OpcUa_NodeClass_View          = 128
             * */
            browseContext.nodeClassMask =
                    OpcUa_NodeClass_ObjectType |
                    OpcUa_NodeClass_Object |
                    OpcUa_NodeClass_Variable |
                    OpcUa_NodeClass_VariableType
                ;
            browseContext.resultMask = OpcUa_BrowseResultMask_All ;
            return browseContext;
        }


        void OpcUaClient::initializeUpdateNamespaceCache(std::vector<std::string> &notFoundObjectTypeNamespaces) {
            m_opcUaWrapper->SessionUpdateNamespaceTable();

            m_uriToIndexCache.clear();
            m_indexToUriCache.clear();
            m_availableObjectTypeNamespaces.clear();

            for (std::size_t i = 0; i < m_expectedObjectTypeNamespaces.size(); i++) {
                notFoundObjectTypeNamespaces.push_back(m_expectedObjectTypeNamespaces[i]);
            }
        }

        void OpcUaClient::connectionStatusChanged(OpcUa_UInt32 /*clientConnectionId*/, UaClientSdk::UaClient::ServerStatus serverStatus)
		{
			switch (serverStatus)
			{
			case UaClientSdk::UaClient::Disconnected:
				LOG(ERROR) << "Disconnected." << std::endl;
				m_isConnected = false;
				break;
			case UaClientSdk::UaClient::Connected:
				LOG(ERROR) << "Connected." << std::endl;
				m_isConnected = true;
				on_connected();
				break;
			case UaClientSdk::UaClient::ConnectionWarningWatchdogTimeout:
				LOG(ERROR) << "ConnectionWarningWatchdogTimeout." << std::endl;
				break;
			case UaClientSdk::UaClient::ConnectionErrorApiReconnect:
				LOG(ERROR) << "ConnectionErrorApiReconnect." << std::endl;
				break;
			case UaClientSdk::UaClient::ServerShutdown:
				LOG(ERROR) << "ServerShutdown." << std::endl;
				break;
			case UaClientSdk::UaClient::NewSessionCreated:
				LOG(ERROR) << "NewSessionCreated." << std::endl;
				break;
			}
		}

		OpcUaClient::~OpcUaClient()
		{
			// Destroy all sessions before UaPlatformLayer::cleanup(); is called!
			m_tryConnecting = false;
			if (m_connectThread)
			{
				m_connectThread->join();
			}

			m_pSession = nullptr;

			if (--PlattformLayerInitialized == 0)
			{
				UaPlatformLayer::cleanup();
			}
		}

		bool OpcUaClient::disconnect()
		{
			if (m_pSession)
			{
				//Subscr.deleteSubscription(m_pSession);
				UaClientSdk::ServiceSettings servsettings;
				return m_opcUaWrapper->SessionDisconnect(servsettings, OpcUa_True).isGood() != OpcUa_False;
			}

			return true;
		}

		void OpcUaClient::threadConnectExecution()
		{
			while (m_tryConnecting)
			{
				if (!m_isConnected)
				{
					this->connect();
				}
				else
				{
					std::this_thread::sleep_for(std::chrono::seconds(1));
				}
			}
		}

		std::list < ModelOpcUa::BrowseResult_t > OpcUaClient::Browse(
			ModelOpcUa::NodeId_t startNode,
			ModelOpcUa::NodeId_t referenceTypeId,
			ModelOpcUa::NodeId_t typeDefinition)
		{
            UaClientSdk::BrowseContext browseContext = prepareBrowseContext(referenceTypeId);
            return BrowseWithContext(startNode, referenceTypeId, typeDefinition, browseContext);
        }

        std::list<ModelOpcUa::BrowseResult_t>
        OpcUaClient::BrowseWithContext(const ModelOpcUa::NodeId_t &startNode,
                                       const ModelOpcUa::NodeId_t &referenceTypeId,
                                       const ModelOpcUa::NodeId_t &typeDefinition,
                                       UaClientSdk::BrowseContext &browseContext) {
            checkConnection();
            auto startUaNodeId = Converter::ModelNodeIdToUaNodeId(startNode, m_uriToIndexCache).getNodeId();
            auto referenceTypeUaNodeId = Converter::ModelNodeIdToUaNodeId(referenceTypeId, m_uriToIndexCache).getNodeId();
            auto typeDefinitionUaNodeId = Converter::ModelNodeIdToUaNodeId(typeDefinition, m_uriToIndexCache).getNodeId();


            OpcUa_NodeClass nodeClass = readNodeClass(typeDefinitionUaNodeId);

            switch (nodeClass)
            {
            case OpcUa_NodeClass_ObjectType:
            {
                browseContext.nodeClassMask = OpcUa_NodeClass_Object;
                break;
            }
            case OpcUa_NodeClass_VariableType:
            {
                browseContext.nodeClassMask = OpcUa_NodeClass_Variable;
                break;
            }
            default:
                LOG(ERROR) << "Invalid NodeClass " << nodeClass
                    << " expect object or variable type for node " << static_cast<std::string>(typeDefinition);
                throw Exceptions::UmatiException("Invalid NodeClass");
            }

            UaByteString continuationPoint;
            UaReferenceDescriptions referenceDescriptions;
            auto uaResult = m_opcUaWrapper->SessionBrowse(m_defaultServiceSettings, startUaNodeId, browseContext, continuationPoint, referenceDescriptions);

            if (uaResult.isBad())
            {
                LOG(ERROR) << "Bad return from browse: " << uaResult.toString().toUtf8();
                throw Exceptions::OpcUaNonGoodStatusCodeException(uaResult);
            }

            std::__cxx11::list < ModelOpcUa::BrowseResult_t > browseResult;
            ReferenceDescriptionsToBrowseResults(typeDefinitionUaNodeId, referenceDescriptions, browseResult);
            handleContinuationPoint(continuationPoint);

            return browseResult;
        }

        void OpcUaClient::ReferenceDescriptionsToBrowseResults(const UaNodeId &typeDefinitionUaNodeId,
                                                               const UaReferenceDescriptions &referenceDescriptions,
                                                               std::list<ModelOpcUa::BrowseResult_t> &browseResult) {
            for (OpcUa_UInt32 i = 0; i < referenceDescriptions.length(); i++)
            {
                auto browseTypeNodeId = UaNodeId(UaExpandedNodeId(referenceDescriptions[i].TypeDefinition).nodeId());
                if (!isSameOrSubtype(typeDefinitionUaNodeId, browseTypeNodeId))
                {
                    continue;
                }

                auto entry = ReferenceDescriptionToBrowseResult(referenceDescriptions[i]);

                browseResult.push_back(entry);
            }
        }

        void OpcUaClient::handleContinuationPoint(const UaByteString &continuationPoint) const {
		    //todo handle continuation point
            LOG(INFO) << "Handling continuation point not yet implemented";
        }

        ModelOpcUa::BrowseResult_t OpcUaClient::ReferenceDescriptionToBrowseResult(const OpcUa_ReferenceDescription &referenceDescription) {
            ModelOpcUa::BrowseResult_t entry;
            auto browseTypeNodeId = UaNodeId(UaExpandedNodeId(referenceDescription.TypeDefinition).nodeId());
            entry.NodeClass = Converter::UaNodeClassToModelNodeClass(referenceDescription.NodeClass).getNodeClass();
            entry.TypeDefinition = Converter::UaNodeIdToModelNodeId(browseTypeNodeId,m_indexToUriCache).getNodeId();
            entry.NodeId = Converter::UaNodeIdToModelNodeId(UaNodeId(UaExpandedNodeId(referenceDescription.NodeId).nodeId()),m_indexToUriCache).getNodeId();
            entry.ReferenceTypeId = Converter::UaNodeIdToModelNodeId(UaNodeId(referenceDescription.ReferenceTypeId),m_indexToUriCache).getNodeId();
            entry.BrowseName = Converter::UaQualifiedNameToModelQualifiedName(referenceDescription.BrowseName,m_indexToUriCache).getQualifiedName();

            return entry;
        }

        UaClientSdk::BrowseContext OpcUaClient::prepareBrowseContext(ModelOpcUa::NodeId_t referenceTypeId) {
            auto referenceTypeUaNodeId = Converter::ModelNodeIdToUaNodeId(referenceTypeId, m_uriToIndexCache).getNodeId();
            UaClientSdk::BrowseContext browseContext;
            browseContext.browseDirection = OpcUa_BrowseDirection_Forward;
            browseContext.includeSubtype = OpcUa_True;
            browseContext.maxReferencesToReturn = 0;
            browseContext.nodeClassMask = 0; // ALL
            if (nullptr != referenceTypeUaNodeId) {
                browseContext.referenceTypeId = referenceTypeUaNodeId;
            }
            browseContext.resultMask =
                OpcUa_BrowseResultMask_BrowseName |
                OpcUa_BrowseResultMask_TypeDefinition |
                OpcUa_BrowseResultMask_NodeClass |
                OpcUa_BrowseResultMask_ReferenceTypeId;
            return browseContext;
        }

        ModelOpcUa::NodeId_t OpcUaClient::TranslateBrowsePathToNodeId(ModelOpcUa::NodeId_t startNode, ModelOpcUa::QualifiedName_t browseName)
		{
			checkConnection();

			if (browseName.isNull())
			{
				LOG(ERROR) << "browseName is NULL";
				throw std::invalid_argument("browseName is NULL");
			}

			if (startNode.isNull())
			{
				LOG(ERROR) << "startNode is NULL";
				throw std::invalid_argument("startNode is NULL");
			}

			auto startUaNodeId = Converter::ModelNodeIdToUaNodeId(startNode, m_uriToIndexCache).getNodeId();
			auto uaBrowseName = Converter::ModelQualifiedNameToUaQualifiedName(browseName, m_uriToIndexCache).getQualifiedName();

			UaRelativePathElements uaBrowsePathElements;
			uaBrowsePathElements.create(1);
			uaBrowsePathElements[0].IncludeSubtypes = OpcUa_True;
			uaBrowsePathElements[0].IsInverse = OpcUa_False;
			uaBrowsePathElements[0].ReferenceTypeId.Identifier.Numeric = OpcUaId_HierarchicalReferences;
			uaBrowseName.copyTo(&uaBrowsePathElements[0].TargetName);

			UaBrowsePaths uaBrowsePaths;
			uaBrowsePaths.create(1);
			uaBrowsePaths[0].RelativePath.NoOfElements = uaBrowsePathElements.length();
			uaBrowsePaths[0].RelativePath.Elements = uaBrowsePathElements.detach();
			startUaNodeId.copyTo(&uaBrowsePaths[0].StartingNode);

			UaBrowsePathResults uaBrowsePathResults;
			UaDiagnosticInfos uaDiagnosticInfos;

			auto uaResult = m_opcUaWrapper->SessionTranslateBrowsePathsToNodeIds(
				m_defaultServiceSettings,
				uaBrowsePaths,
				uaBrowsePathResults,
				uaDiagnosticInfos
			);

			if (uaResult.isBad())
			{
				LOG(ERROR) << "TranslateBrowsePathToNodeId failed for node: '" << static_cast<std::string>(startNode)
					<< "' with " << uaResult.toString().toUtf8() << "(BrowsePath: " << static_cast<std::string>(browseName) << ")";
				throw Exceptions::OpcUaNonGoodStatusCodeException(uaResult);
			}

			if (uaBrowsePathResults.length() != 1)
			{
				LOG(ERROR) << "Expect 1 browseResult, got " << uaBrowsePathResults.length() << " for node: '" << static_cast<std::string>(startNode)
					<< "' with " << uaResult.toString().toUtf8() << "(BrowsePath: " << static_cast<std::string>(browseName) << ")";
				throw Exceptions::UmatiException("BrowseResult length mismatch.");
			}

			UaStatusCode uaResultElement(uaBrowsePathResults[0].StatusCode);
			if (uaResultElement.isBad())
			{
				LOG(ERROR) << "Element returned bad status code: " << uaResultElement.toString().toUtf8() << " for node: '" << static_cast<std::string>(startNode)
					<< "' with " << uaResult.toString().toUtf8() << "(BrowsePath: " << static_cast<std::string>(browseName) << ")";
				throw Exceptions::OpcUaNonGoodStatusCodeException(uaResultElement);
			}

			if (uaBrowsePathResults[0].NoOfTargets != 1)
			{
				LOG(ERROR) << "Expect 1 traget, got " << uaBrowsePathResults[0].NoOfTargets << " for node: '" << static_cast<std::string>(startNode)
					<< "' with " << uaResult.toString().toUtf8() << "(BrowsePath: " << static_cast<std::string>(browseName) << ")";
				throw Exceptions::UmatiException("Number of targets mismatch.");
			}

			UaNodeId targetNodeId(UaExpandedNodeId(uaBrowsePathResults[0].Targets[0].TargetId).nodeId());

			return Converter::UaNodeIdToModelNodeId(targetNodeId, m_indexToUriCache).getNodeId();
		}

		std::shared_ptr<Dashboard::IDashboardDataClient::ValueSubscriptionHandle> OpcUaClient::Subscribe(ModelOpcUa::NodeId_t nodeId, newValueCallbackFunction_t callback)
		{
			return m_opcUaWrapper->SubscriptionSubscribe(nodeId, callback);
		}

		std::vector<nlohmann::json> OpcUaClient::readValues(std::list<ModelOpcUa::NodeId_t> modelNodeIds)
		{
			UaStatus uaStatus;
			//std::list <UaNodeId> readNodeIds;
			UaReadValueIds readValueIds;
			readValueIds.resize(modelNodeIds.size());
			unsigned int i = 0;
			for (const auto &modelNodeId : modelNodeIds)
			{
				UaNodeId nodeId = Converter::ModelNodeIdToUaNodeId(modelNodeId, m_uriToIndexCache).getNodeId();
				nodeId.copyTo(&(readValueIds[i].NodeId));
				readValueIds[i].AttributeId = OpcUa_Attributes_Value;
				++i;
			}

			UaDataValues readValues;
			UaDiagnosticInfos diagnosticInfos;

			uaStatus = m_opcUaWrapper->SessionRead(
				m_defaultServiceSettings,
				0,
				OpcUa_TimestampsToReturn::OpcUa_TimestampsToReturn_Neither,
				readValueIds,
				readValues,
				diagnosticInfos);

			if (uaStatus.isNotGood())
			{
				LOG(ERROR) << "Received non good status for read: " << uaStatus.toString().toUtf8();
				std::stringstream ss;
				ss << "Received non good status  for read: " << uaStatus.toString().toUtf8();
				throw Exceptions::OpcUaException(ss.str());
			}

			std::vector<nlohmann::json> ret;

			for (i = 0; i < readValues.length(); ++i)
			{
				auto value = readValues[i];
				ret.push_back(Converter::UaDataValueToJsonValue(value).getValue());
			}
			return ret;
		}

        void OpcUaClient::createTypeMap(std::shared_ptr<std::map<std::string, std::shared_ptr<ModelOpcUa::StructureBiNode>>> &bidirectionalTypeMap, std::shared_ptr<std::map<std::string, ModelOpcUa::StructureNode>> typeMap, uint16_t namespaceIndex) {
            for ( auto typeIterator = bidirectionalTypeMap->begin(); typeIterator != bidirectionalTypeMap->end(); typeIterator++ ) {
                if(typeIterator->second->namespaceIndex != namespaceIndex) {
                    continue;
                }
                // go to highest parent and then down the ladder to add / update attributes;
                // create a list of pointers till parent is null
                // go backwards and add / update child nodes till the end
                std::list<std::shared_ptr<ModelOpcUa::StructureBiNode>> bloodline;
                std::shared_ptr<ModelOpcUa::StructureBiNode> currentGeneration = typeIterator->second;
                while(nullptr != currentGeneration) {
                    bloodline.emplace_back(currentGeneration);
                    currentGeneration = currentGeneration->parent;
                }
                std::string typeName = bloodline.front()->structureNode->SpecifiedBrowseName.Name;
                ModelOpcUa::StructureNode node = bloodline.front()->structureNode.operator*();
                for(auto bloodlineIterator = bloodline.end(); bloodlineIterator != bloodline.begin(); bloodlineIterator--){
                    auto ancestor = bloodlineIterator->get();
                    for(auto childIterator = ancestor->SpecifiedBiChildNodes.begin(); childIterator != ancestor->SpecifiedBiChildNodes.end(); childIterator++) {
                        LOG(INFO) << "Ping!";
                        //node.SpecifiedChildNodes.emplace_back()
                    }
                }


                std::pair <std::string, ModelOpcUa::StructureNode> newType(typeName, node);
                typeMap->insert(newType);
            }
        }
    }
}