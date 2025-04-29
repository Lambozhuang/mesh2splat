///////////////////////////////////////////////////////////////////////////////
//         Mesh2Splat: fast mesh to 3D gaussian splat conversion             //
//        Copyright (c) 2025 Electronic Arts Inc. All rights reserved.       //
///////////////////////////////////////////////////////////////////////////////

#include "renderer.hpp"
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

static std::vector<utils::GaussianDataSSBO> sampleTriangleCPU_Internal(
    const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2,
    int m /* sampling density */, float scaleFactor /* New parameter */) // Add scaleFactor
{
    std::vector<utils::GaussianDataSSBO> out;
    if (m <= 0) return out; // Avoid division by zero and unnecessary work

    out.reserve((m + 1) * (m + 2) / 2);

    glm::vec3 e1 = p1 - p0;
    glm::vec3 e2 = p2 - p0;
    glm::vec3 n = glm::normalize(glm::cross(e1, e2));

    // Check for degenerate triangles
    if (glm::length(n) < 1e-6f) {
        return out; // Skip degenerate triangles
    }

    // Orthonormal basis X,Y,Z (Z = normal)
    glm::vec3 X = glm::normalize(e1);
    // Handle potential collinearity of e1 and e2 leading to zero cross product
    glm::vec3 Y_candidate = glm::cross(n, X);
    if (glm::length(Y_candidate) < 1e-6f) {
         // If n and X are parallel (e.g., e1 is zero length), create an arbitrary orthogonal vector
        glm::vec3 arbitrary_non_parallel = (abs(n.x) < 0.9f) ? glm::vec3(1,0,0) : glm::vec3(0,1,0);
        Y_candidate = glm::normalize(glm::cross(n, arbitrary_non_parallel));
        X = glm::normalize(glm::cross(Y_candidate, n)); // Recompute X to ensure orthogonality
    }
     glm::vec3 Y = glm::normalize(Y_candidate);


    glm::mat3 basis(X, Y, n);
    glm::quat Q = glm::quat_cast(basis);

    // Apply scaleFactor here
    float su = (glm::length(e1) / float(m)) * scaleFactor;
    // Perpendicular component of e2 w.r.t X for isotropy in tangent plane
    glm::vec3 e2_perp = e2 - glm::dot(e2, X) * X;
    float sv = (glm::length(e2_perp) / float(m)) * scaleFactor;

    // --- Make Gaussians isotropic (circular) ---
    float avg_scale = (su + sv) * 0.5f; // Calculate average scale
    // Use a small epsilon for scale, avoid zero or negative scales
    // Use avg_scale for both X and Y components
    glm::vec3 S(avg_scale > 1e-7f ? avg_scale : 1e-7f,
                avg_scale > 1e-7f ? avg_scale : 1e-7f,
                1e-7f); // Keep Z scale minimal

    for (int u = 0; u <= m; ++u)
    {
        for (int v = 0; v <= m - u; ++v)
        {
            float fu = float(u) / m;
            float fv = float(v) / m;
            float fw = 1.0f - fu - fv;

            glm::vec3 P = fw * p0 + fu * p1 + fv * p2;

            utils::GaussianDataSSBO g;
            g.position = glm::vec4(P, 1.0f);
            g.scale = glm::vec4(S, 0.0f); // Use pre-calculated scale S
            g.normal = glm::vec4(n, 0.0f);
            g.rotation = glm::vec4(Q.w, Q.x, Q.y, Q.z);
            g.color = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
            g.pbr = glm::vec4(0.0f, 0.5f, 0.0f, 0.0f);

            out.push_back(g);
        }
    }
    return out;
}

//TODO: create a separete camera class, avoid it bloating and getting too messy

Renderer::Renderer(GLFWwindow* window, Camera& cameraInstance) : camera(cameraInstance), renderContext {}
{

    rendererGlfwWindow = window;
    renderPassesOrder = {};

    sceneManager = std::make_unique<SceneManager>(renderContext);
    renderContext.gaussianBuffer                = 0;
    renderContext.gaussianDepthPostFiltering    = 0;
    renderContext.drawIndirectBuffer            = 0;
    renderContext.keysBuffer                    = 0;
	renderContext.valuesBuffer                  = 0;
    renderContext.perQuadTransformationBufferSorted = 0;
    renderContext.perQuadTransformationsBuffer  = 0;
    renderContext.atomicCounterBuffer = 0;
    renderContext.atomicCounterBufferConversionPass = 0;

    renderContext.normalizedUvSpaceWidth        = 0;
    renderContext.normalizedUvSpaceHeight       = 0;
    renderContext.rendererGlfwWindow            = window; //TODO: this double reference is ugly, refactor

    
    lastShaderCheckTime      = glfwGetTime();

    //TODO: should this maybe live in the Renderer rather than shader utils? Probably yes
    glUtils::initializeShaderLocations();
    
    glUtils::initializeShaderFileMonitoring(renderContext.shaderRegistry);

    updateShadersIfNeeded(true); //Forcing compilation
    
    glGenVertexArrays(1, &(renderContext.vao));
    
    glGenBuffers(1, &(renderContext.keysBuffer));
    glGenBuffers(1, &(renderContext.perQuadTransformationsBuffer));
    glGenBuffers(1, &(renderContext.valuesBuffer));
    glGenBuffers(1, &(renderContext.perQuadTransformationBufferSorted));
    glGenBuffers(1, &(renderContext.gaussianDepthPostFiltering));
    glGenBuffers(1, &(renderContext.gaussianBuffer));

    glUtils::resizeAndBindToPosSSBO<glm::vec4>(MAX_GAUSSIANS_TO_SORT * 6, renderContext.gaussianBuffer, 0);
    glUtils::resizeAndBindToPosSSBO<unsigned int>(MAX_GAUSSIANS_TO_SORT, renderContext.keysBuffer, 1);
    glUtils::resizeAndBindToPosSSBO<unsigned int>(MAX_GAUSSIANS_TO_SORT, renderContext.valuesBuffer, 2);
    glUtils::resizeAndBindToPosSSBO<glm::vec4>(MAX_GAUSSIANS_TO_SORT * 6, renderContext.perQuadTransformationBufferSorted, 3);
    glUtils::resizeAndBindToPosSSBO<glm::vec4>(MAX_GAUSSIANS_TO_SORT * 6, renderContext.perQuadTransformationsBuffer, 4);
    glUtils::resizeAndBindToPosSSBO<float>(MAX_GAUSSIANS_TO_SORT, renderContext.gaussianDepthPostFiltering, 5);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    for (size_t i = 0; i < 10; ++i) {
        GLuint query;
        glGenQueries(1, &query);
        renderContext.queryPool.push_back(query);
    }

    // First atomic counter
    glGenBuffers(1, &renderContext.atomicCounterBuffer);
    glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, renderContext.atomicCounterBuffer);
    glBufferData(GL_ATOMIC_COUNTER_BUFFER, sizeof(GLuint), nullptr, GL_DYNAMIC_DRAW);

    GLuint zeroVal = 0;
    glBufferSubData(GL_ATOMIC_COUNTER_BUFFER, 0, sizeof(GLuint), &zeroVal);
    glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, 0);

    // Second atomic counter
    glGenBuffers(1, &renderContext.atomicCounterBufferConversionPass);
    glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, renderContext.atomicCounterBufferConversionPass);
    glBufferData(GL_ATOMIC_COUNTER_BUFFER, sizeof(GLuint), nullptr, GL_DYNAMIC_DRAW);
    glBufferSubData(GL_ATOMIC_COUNTER_BUFFER, 0, sizeof(GLuint), &zeroVal);
    glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, 0);

    //Indirect buff
    glGenBuffers(1, &(renderContext.drawIndirectBuffer));
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, renderContext.drawIndirectBuffer);
    glBufferData(GL_DRAW_INDIRECT_BUFFER,
                    sizeof(IRenderPass::DrawElementsIndirectCommand),
                    nullptr,
                    GL_DYNAMIC_DRAW);

    IRenderPass::DrawElementsIndirectCommand cmd_init;
    cmd_init.count         = 6;  
    cmd_init.instanceCount = 0;
    cmd_init.first         = 0;
    cmd_init.baseVertex    = 0;
    cmd_init.baseInstance  = 0;

    glBufferSubData(GL_DRAW_INDIRECT_BUFFER, 0, sizeof(IRenderPass::DrawElementsIndirectCommand), &cmd_init);

}

Renderer::~Renderer()
{
    glDeleteVertexArrays(1, &(renderContext.vao));

    glDeleteBuffers(1, &(renderContext.gaussianBuffer));
    glDeleteBuffers(1, &(renderContext.drawIndirectBuffer));
    glDeleteBuffers(1, &(renderContext.keysBuffer));
    glDeleteBuffers(1, &(renderContext.valuesBuffer));
    glDeleteBuffers(1, &(renderContext.perQuadTransformationBufferSorted));
    glDeleteBuffers(1, &(renderContext.gaussianDepthPostFiltering));

    for (auto& query : renderContext.queryPool) {
        glDeleteQueries(1, &query);
    }
}

void Renderer::initialize() {

    renderPasses[conversionPassName]                    = std::make_unique<ConversionPass>();
    renderPasses[depthPrepassName]                      = std::make_unique<DepthPrepass>();
    renderPasses[gaussiansPrePassName]                  = std::make_unique<GaussiansPrepass>();
    renderPasses[radixSortPassName]                     = std::make_unique<RadixSortPass>();
    renderPasses[gaussianSplattingPassName]             = std::make_unique<GaussianSplattingPass>(renderContext);
    renderPasses[gaussianSplattingRelightingPassName]   = std::make_unique<GaussianRelightingPass>();
    renderPasses[gaussianSplattingShadowsPassName]      = std::make_unique<GaussianShadowPass>(renderContext);

    renderPassesOrder = {
        conversionPassName,
        depthPrepassName,
        gaussiansPrePassName,
        radixSortPassName,
        gaussianSplattingPassName,
        gaussianSplattingShadowsPassName,
        gaussianSplattingRelightingPassName
    };

    createDepthTexture();
    createGBuffer();
}

void Renderer::renderFrame()
{
    if (!renderContext.queryPool.empty()) {
        GLuint currentQuery = renderContext.queryPool.front();
        glBeginQuery(GL_TIME_ELAPSED, currentQuery);
    }

    for (auto& renderPassName : renderPassesOrder)
    {
        auto& passPtr = renderPasses[renderPassName];
        if (passPtr->isEnabled())
        {
            passPtr->execute(renderContext);
            passPtr->setIsEnabled(false); //Default to false for next frame
        }
    }

    if (!renderContext.queryPool.empty()) {
        GLuint currentQuery = renderContext.queryPool.front();
        glEndQuery(GL_TIME_ELAPSED);
    
        renderContext.queryPool.pop_front();
        renderContext.queryPool.push_back(currentQuery);
    }
    
    if (renderContext.queryPool.size() > 5) {
        GLuint completedQuery = renderContext.queryPool.front();
        GLuint64 elapsedTime = 0;
        glGetQueryObjectui64v(completedQuery, GL_QUERY_RESULT, &elapsedTime);
        this->gpuFrameTimeMs = static_cast<double>(elapsedTime) / 1e6; // ns to ms
    }
};        

void Renderer::updateTransformations()
{

    int width, height;
    glfwGetFramebufferSize(rendererGlfwWindow, &width, &height);

    float fov = camera.GetFOV();

    renderContext.nearPlane = 0.01f;
    renderContext.farPlane = 100.0f;

    renderContext.projMat = glm::perspective(glm::radians(fov),
                                            (float)width / (float)height,
                                            renderContext.nearPlane, renderContext.farPlane);
    // Set viewport
    glViewport(0, 0, width, height);

    // Use Camera's view matrix
    renderContext.viewMat = camera.GetViewMatrix();

    renderContext.MVP = renderContext.projMat * renderContext.viewMat * renderContext.modelMat;

    float htany = tan(glm::radians(fov) / 2);
    float htanx = htany / height * width;
    float focal_z = height / (2 * htany);
    renderContext.hfov_focal = glm::vec3(htanx, htany, focal_z);

    renderContext.camPos = camera.GetPosition();
}

void Renderer::clearingPrePass(glm::vec4 clearColor)
{
    glClearColor(clearColor.r, clearColor.g, clearColor.b, 0); //alpha==0 Important for correct blending --> but still front to back expects first DST to be (0,0,0,0)
    //TODO: find way to circumvent first write, as bkg color should not be accounted for in blending
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void Renderer::setLastShaderCheckTime(double lastShaderCheckedTime)
{
    this->lastShaderCheckTime = lastShaderCheckedTime;
}

double Renderer::getLastShaderCheckTime()
{
    return lastShaderCheckTime;
}

RenderContext* Renderer::getRenderContext()
{
    return &renderContext;
}

void Renderer::enableRenderPass(std::string renderPassName)
{
    if (auto renderPass = renderPasses.find(renderPassName); renderPass != renderPasses.end())
    {
        renderPass->second->setIsEnabled(true);
    } else {
        std::cerr << "RenderPass: [ "<< renderPassName << " ] not found." << std::endl;
    }
}

void Renderer::setViewportResolutionForConversion(int resolutionTarget)
{
    std::cout<<"Resolution target: " << int(resolutionTarget / renderContext.dataMeshAndGlMesh.size()) << std::endl;
    renderContext.resolutionTarget = int(resolutionTarget / renderContext.dataMeshAndGlMesh.size());
}

            
void Renderer::setFormatType(unsigned int format)
{
    renderContext.format = format;
};

void Renderer::resetRendererViewportResolution()
{
    int width, height;
    glfwGetFramebufferSize(rendererGlfwWindow, &width, &height);
    renderContext.rendererResolution = glm::ivec2(width, height);
}

void Renderer::setStdDevFromImGui(float stdDev)
{
    renderContext.gaussianStd = stdDev;
}

void Renderer::setRenderMode(ImGuiUI::VisualizationOption selectedRenderMode)
{
    //HM: https://stackoverflow.com/questions/14589417/can-an-enum-class-be-converted-to-the-underlying-type (I just one-lined it)
    renderContext.renderMode = static_cast<std::underlying_type<ImGuiUI::VisualizationOption>::type>(selectedRenderMode);
}

SceneManager& Renderer::getSceneManager()
{
    return *sceneManager;
}

double Renderer::getTotalGpuFrameTimeMs() const { return gpuFrameTimeMs; }

void Renderer::resetModelMatrices()
{
    renderContext.modelMat = glm::mat4(1.0f);
}

void Renderer::createDepthTexture()
{
    resetRendererViewportResolution();
    glGenFramebuffers(1, &renderContext.depthFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, renderContext.depthFBO);

    glGenTextures(1, &renderContext.meshDepthTexture);
    glBindTexture(GL_TEXTURE_2D, renderContext.meshDepthTexture);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, renderContext.rendererResolution.x, renderContext.rendererResolution.y, 0, GL_DEPTH_COMPONENT, GL_FLOAT, 0);
    
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); 
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);

   glFramebufferTexture2D(
       GL_FRAMEBUFFER,
       GL_DEPTH_ATTACHMENT,
       GL_TEXTURE_2D,
       renderContext.meshDepthTexture,
       0
   );
   
   glDrawBuffer(GL_NONE);
   glReadBuffer(GL_NONE);
}

void Renderer::deleteDepthTexture()
{
    glDeleteFramebuffers(1, &renderContext.depthFBO);
    glDeleteTextures(1, &renderContext.meshDepthTexture);

    renderContext.depthFBO          = 0;
    renderContext.meshDepthTexture  = 0;
}

void Renderer::setDepthTestEnabled(bool depthTest)
{
    renderContext.performMeshDepthTest = depthTest;
}

void Renderer::createGBuffer()
{
    resetRendererViewportResolution();

    glGenFramebuffers(1, &renderContext.gBufferFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, renderContext.gBufferFBO);

    // G-Buffer
    glGenTextures(1, &renderContext.gPosition);
    glBindTexture(GL_TEXTURE_2D, renderContext.gPosition);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, renderContext.rendererResolution.x, renderContext.rendererResolution.y, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, renderContext.gPosition, 0);

    //I need to blend this so I need the alpha
    glGenTextures(1, &renderContext.gNormal);
    glBindTexture(GL_TEXTURE_2D, renderContext.gNormal);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, renderContext.rendererResolution.x, renderContext.rendererResolution.y, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, renderContext.gNormal, 0);

    glGenTextures(1, &renderContext.gAlbedo);
    glBindTexture(GL_TEXTURE_2D, renderContext.gAlbedo);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, renderContext.rendererResolution.x, renderContext.rendererResolution.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, renderContext.gAlbedo, 0);

    //I need to use my own blending function for depth, (more like: I want to)
    glGenTextures(1, &renderContext.gDepth);
    glBindTexture(GL_TEXTURE_2D, renderContext.gDepth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, renderContext.rendererResolution.x, renderContext.rendererResolution.y, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3, GL_TEXTURE_2D, renderContext.gDepth, 0);

    glGenTextures(1, &renderContext.gMetallicRoughness);
    glBindTexture(GL_TEXTURE_2D, renderContext.gMetallicRoughness);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, renderContext.rendererResolution.x, renderContext.rendererResolution.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT4, GL_TEXTURE_2D, renderContext.gMetallicRoughness, 0);
    

    std::vector<GLenum> attachments = {
        GL_COLOR_ATTACHMENT0,
        GL_COLOR_ATTACHMENT1,
        GL_COLOR_ATTACHMENT2,
        GL_COLOR_ATTACHMENT3,
        GL_COLOR_ATTACHMENT4
    };

    glDrawBuffers(static_cast<GLsizei>(attachments.size()), attachments.data());

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        std::cerr << "GBuffer FBO not complete!" << std::endl;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::deleteGBuffer()
{
    glDeleteFramebuffers(1, &renderContext.gBufferFBO);
    glDeleteTextures(1, &renderContext.gPosition);
    glDeleteTextures(1, &renderContext.gNormal);
    glDeleteTextures(1, &renderContext.gAlbedo);
    glDeleteTextures(1, &renderContext.gDepth);
    glDeleteTextures(1, &renderContext.gMetallicRoughness);

    renderContext.gBufferFBO    = 0;
    renderContext.gPosition     = 0;
    renderContext.gNormal       = 0;
    renderContext.gAlbedo       = 0;
    renderContext.gDepth        = 0;
}

	
void Renderer::setLightingEnabled(bool isEnabled)
{
    renderContext.pointLightData.lightingEnabled = isEnabled;
}

void Renderer::setLightIntensity(float lightIntensity)
{
    renderContext.pointLightData.lightIntensity = lightIntensity;
}

void Renderer::setLightColor(glm::vec3 lightColor)
{
    renderContext.pointLightData.lightColor = lightColor;
}

bool Renderer::hasWindowSizeChanged()
{
    int width, height;
    glfwGetFramebufferSize(rendererGlfwWindow, &width, &height);
    return renderContext.rendererResolution != glm::ivec2(width, height);
}

bool Renderer::isWindowMinimized()
{
    return glfwGetWindowAttrib(rendererGlfwWindow, GLFW_ICONIFIED);
}


void Renderer::updateGaussianBuffer()
{
    glUtils::fillGaussianBufferSsbo(renderContext.gaussianBuffer, renderContext.readGaussians);
    int buffSize = 0;
    glBindBuffer          (GL_SHADER_STORAGE_BUFFER, renderContext.gaussianBuffer);
    glGetBufferParameteriv(GL_SHADER_STORAGE_BUFFER, GL_BUFFER_SIZE, &buffSize);
    renderContext.numberOfGaussians = buffSize / sizeof(utils::GaussianDataSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void Renderer::gaussianBufferFromSize(unsigned int size)
{
    glUtils::fillGaussianBufferSsbo(renderContext.gaussianBuffer, size);
}

bool Renderer::updateShadersIfNeeded(bool forceReload) {
    return renderContext.shaderRegistry.reloadModifiedShaders(forceReload);
}

unsigned int Renderer::getVisibleGaussianCount()
{
    if (this->renderContext.atomicCounterBuffer)
    {
        uint32_t validCount = 0;
        glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, renderContext.atomicCounterBuffer);
        glGetBufferSubData(GL_ATOMIC_COUNTER_BUFFER, 0, sizeof(uint32_t), &validCount);
        
        return validCount;
    }
    return 0;

}

unsigned int Renderer::getTotalGaussianCount()
{
    return this->renderContext.numberOfGaussians;

}

void Renderer::convertMeshToGaussiansCPU(int samplingDensity, float scaleFactor) // Add scaleFactor
{
    std::cout << "Starting CPU Mesh to Gaussian Conversion..." << std::endl;
    renderContext.readGaussians.clear();

    for (const auto& meshPair : renderContext.dataMeshAndGlMesh)
    {
        const utils::Mesh& mesh = meshPair.first;

        if (mesh.faces.empty()) {
            std::cerr << "Warning: Skipping mesh with no faces." << std::endl;
            continue;
        }

        // Iterate through each face (triangle) in the mesh
        for (const auto& face : mesh.faces)
        {
            // Extract the three vertex positions for the current face
            const glm::vec3& p0 = face.pos[0];
            const glm::vec3& p1 = face.pos[1];
            const glm::vec3& p2 = face.pos[2];

            // Generate Gaussians for this triangle, passing the scaleFactor
            std::vector<utils::GaussianDataSSBO> triangleGaussians = sampleTriangleCPU_Internal(p0, p1, p2, samplingDensity, scaleFactor);

            // Add the generated Gaussians to the main list
            renderContext.readGaussians.insert(renderContext.readGaussians.end(), triangleGaussians.begin(), triangleGaussians.end());
        }
    }

    std::cout << "CPU Conversion finished. Generated " << renderContext.readGaussians.size() << " gaussians." << std::endl;

    // Update the GPU buffer with the new CPU-generated data
    updateGaussianBuffer();

    // Optionally, trigger necessary render passes if needed immediately after conversion
    // enableRenderPass(gaussiansPrePassName);
    // enableRenderPass(radixSortPassName);
    // enableRenderPass(gaussianSplattingPassName);
    // enableRenderPass(gaussianSplattingRelightingPassName);
    // resetRendererViewportResolution();
}