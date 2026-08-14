varying mat3 normalToViewMatrix;

mat3 generateTangentSpace(vec4 tangent, vec3 normal)
{
    vec3 normalizedNormal = normalize(normal);
    vec3 normalizedTangent = normalize(tangent.xyz);
<<<<<<< HEAD
    vec3 binormal = cross(normalizedNormal, normalizedTangent) * tangent.w;
=======
    vec3 binormal = cross(normalizedTangent, normalizedNormal) * tangent.w;
>>>>>>> origin/main
    return mat3(normalizedTangent, binormal, normalizedNormal);
}

vec3 normalToView(vec3 normal)
{
    return normalize(normalToViewMatrix * normal);
}
