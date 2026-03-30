for file in exported_linear/*.png; 
do
     filename=$(basename "$file" .png);
     tex3ds -f etc1a4 -z none -o "t3x/${filename}.t3x" "$file";
done

