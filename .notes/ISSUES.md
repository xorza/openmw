# Open issues

- Under the ray tracer the shader visitor is off (`Resource::SceneManager::setShadersEnabled(false)`),
  so with `[Shaders] auto use object normal maps` or `auto use object specular maps` on, the `_n`
  and `_spec` textures the visitor used to find by pattern beside a model's diffuse map are never
  attached, and `Rtx::Surface` reads no normal or specular map for those models. Off by default.
