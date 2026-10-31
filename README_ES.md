# GTA4 Ragdoll Android - Fase 1

Mod AML para GTA SA 2.00 (armeabi-v7a). Esta fase NO es el ragdoll con huesos todavia:
es la capa de reaccion (tambaleo hacia atras segun arma/vehiculo) + un log para ajustar todo.

## Compilar sin PC (GitHub)
1. Crea una cuenta en github.com y un repositorio nuevo (publico o privado).
2. Sube TODOS estos archivos respetando carpetas (main.cpp, Android.mk, Application.mk y
   .github/workflows/build.yml). Desde el navegador: "Add file > Upload files".
3. Pestaña "Actions": se ejecuta solo al subir. Cuando termine (circulo verde), entra a la
   ejecucion y baja el artefacto "libGTA4RagdollAndroid" (es un zip con el .so).
4. Si falla, copia el mensaje de error del paso "Compilar" y mandamelo.

## Instalar
Copia `libGTA4RagdollAndroid.so` a:
`Android/data/com.rockstargames.gtasa/mods/`
Abre el juego una vez; se crea el .ini de configuracion en la carpeta de config de AML.

## Probar
Dispara a un peaton y mira `Android/data/com.rockstargames.gtasa/files/GTA4Ragdoll.log`.
Mandame ese archivo: las lineas "[dmg]" dicen el arma detectada, la fuerza y las posiciones.
Si el .so no carga (el juego se cierra al abrir), dime tu version de AML: las cabeceras deben
coincidir (tu APK trae AML 1.3.0). En el workflow puedes poner la rama/tag correcta en "aml_ref".
